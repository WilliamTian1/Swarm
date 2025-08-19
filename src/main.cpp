// SwarmOverlay expanded prototype
// Provides: multiple virtual cursors with behaviors + named pipe IPC

#include <windows.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <map>
#include <cctype>
#include <sstream>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>

/*
 Swarm prototype
 - Creates overlay window with additional visual cursors
 - Mirrors system cursor position to all swarm cursors (simple prototype)
 - Future: independent scripted behaviors, AHK integration via IPC or shared memory
*/

enum class BehaviorType { Mirror, Static, Orbit, FollowLag };

struct SwarmCursor {
    int id {0};
    BehaviorType behavior {BehaviorType::Mirror};
    POINT pos {0,0}; // current render position
    POINT target {0,0}; // for static / follow
    COLORREF color {RGB(255,0,0)};
    int size {12};
    // behavior params
    double offsetX {0};
    double offsetY {0};
    double radius {60};
    double angle {0};
    double speed {1}; // radians per second for orbit
    double lagMs {120};
    // For FollowLag EMA
    bool initialized {false};
};

class SwarmManager {
public:
    std::vector<SwarmCursor> cursors;
    std::mutex mtx;
    std::atomic<bool> running {true};
    HWND overlayWnd {nullptr};
    std::atomic<int> nextId {1};

    int addCursor(const SwarmCursor &base) {
        std::lock_guard<std::mutex> lock(mtx);
        SwarmCursor c = base;
        if(c.id==0) c.id = nextId++;
        cursors.push_back(c);
        return c.id;
    }
    bool removeCursor(int id) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = std::remove_if(cursors.begin(), cursors.end(), [&](auto &c){return c.id==id;});
        if(it==cursors.end()) return false;
        cursors.erase(it, cursors.end());
        return true;
    }
    std::optional<SwarmCursor> getCursorCopy(int id) {
        std::lock_guard<std::mutex> lock(mtx);
        for(auto &c: cursors) {
            if(c.id==id) return c;
        }
        return std::nullopt;
    }
    void updateAll(double dt, POINT systemPos) {
        std::lock_guard<std::mutex> lock(mtx);
        for(auto &c : cursors) {
            switch(c.behavior) {
                case BehaviorType::Mirror: {
                    c.pos.x = (LONG)(systemPos.x + c.offsetX);
                    c.pos.y = (LONG)(systemPos.y + c.offsetY);
                } break;
                case BehaviorType::Static: {
                    c.pos = c.target;
                } break;
                case BehaviorType::Orbit: {
                    c.angle += c.speed * dt;
                    c.pos.x = (LONG)(systemPos.x + cos(c.angle) * c.radius);
                    c.pos.y = (LONG)(systemPos.y + sin(c.angle) * c.radius);
                } break;
                case BehaviorType::FollowLag: {
                    if(!c.initialized) { c.pos = systemPos; c.initialized=true; }
                    double alpha = dt * 1000.0 / (c.lagMs > 1 ? c.lagMs : 1); // proportion per frame
                    if(alpha>1) alpha=1;
                    c.pos.x = (LONG)(c.pos.x + (systemPos.x - c.pos.x)*alpha);
                    c.pos.y = (LONG)(c.pos.y + (systemPos.y - c.pos.y)*alpha);
                } break;
            }
        }
    }
};

static SwarmManager gManager;
static std::atomic<bool> gSolidMode {false};
static std::atomic<bool> gWindowedMode {false};
static std::mutex gOutPipeMtx;
static HANDLE gOutPipe = INVALID_HANDLE_VALUE; // outbound event stream
static std::atomic<bool> gOutPipeReady {false};
static bool gShowHelp = true; // draw help text overlay in windowed mode for user guidance

void sendOut(const std::string &line) {
    std::lock_guard<std::mutex> lock(gOutPipeMtx);
    if(gOutPipeReady && gOutPipe!=INVALID_HANDLE_VALUE) {
        std::string data = line; if(data.empty() || data.back()!='\n') data.push_back('\n');
        DWORD written=0; WriteFile(gOutPipe, data.data(), (DWORD)data.size(), &written, nullptr);
    }
}

void SwitchToWindowed(HWND hWnd) {
    if(!hWnd) return;
    gWindowedMode = true;
    LONG_PTR ex = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    // remove layered transparency for clarity during debug
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, (ex & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT)) | WS_EX_APPWINDOW);
    LONG_PTR st = GetWindowLongPtr(hWnd, GWL_STYLE);
    SetWindowLongPtr(hWnd, GWL_STYLE, (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    int w = 800, h = 600;
    int sx = (GetSystemMetrics(SM_CXSCREEN)-w)/2;
    int sy = (GetSystemMetrics(SM_CYSCREEN)-h)/2;
    SetWindowPos(hWnd, HWND_TOP, sx, sy, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_FRAME);
    printf("Switched to WINDOWED debug mode.\n");
}

void SwitchToOverlay(HWND hWnd) {
    if(!hWnd) return;
    gWindowedMode = false;
    LONG_PTR st = GetWindowLongPtr(hWnd, GWL_STYLE);
    SetWindowLongPtr(hWnd, GWL_STYLE, (st & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    LONG_PTR ex = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, (ex | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT) & ~WS_EX_APPWINDOW);
    SetLayeredWindowAttributes(hWnd, RGB(0,0,0), 0, LWA_COLORKEY);
    SetWindowPos(hWnd, HWND_TOPMOST, 0,0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_FRAME);
    printf("Switched back to OVERLAY mode.\n");
}

// --- Minimal JSON-ish parser for flat objects (string/number) ---
static std::map<std::string,std::string> parseSimpleJson(const std::string &line) {
    std::map<std::string,std::string> out;
    size_t i=0; while(i<line.size() && isspace((unsigned char)line[i])) i++;
    if(i>=line.size() || line[i] != '{') return out; 
    i++;
    while(i<line.size()) {
        while(i<line.size() && isspace((unsigned char)line[i])) i++;
        if(i<line.size() && line[i]=='}') break;
    if(line[i] != '"') break; 
    i++;
        size_t start=i; while(i<line.size() && line[i] != '"') i++; if(i>=line.size()) break;
        std::string key = line.substr(start, i-start); i++; // skip closing quote
        while(i<line.size() && (isspace((unsigned char)line[i])|| line[i]==':')) { if(line[i]==':'){ i++; break;} i++; }
        while(i<line.size() && isspace((unsigned char)line[i])) i++;
        std::string value;
        if(i<line.size() && line[i]=='"') {
            i++; size_t vstart=i; while(i<line.size() && line[i] != '"') i++; value = line.substr(vstart, i-vstart); if(i<line.size()) i++;
        } else {
            size_t vstart=i; while(i<line.size() && line[i] != ',' && line[i] != '}') i++; value = line.substr(vstart, i-vstart);
            // trim
            size_t a=0; while(a<value.size() && isspace((unsigned char)value[a])) a++; size_t b=value.size(); while(b> a && isspace((unsigned char)value[b-1])) b--; value = value.substr(a,b-a);
        }
        out[key]=value;
        while(i<line.size() && isspace((unsigned char)line[i])) i++;
        if(i<line.size() && line[i]==',') { i++; continue; }
        if(i<line.size() && line[i]=='}') break;
    }
    return out;
}

static COLORREF parseColor(const std::string &s) {
    if(s.size()==7 && s[0]=='#') {
        auto hexVal=[&](char c)->int { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return 10+c-'a'; if(c>='A'&&c<='F') return 10+c-'A'; return 0; };
        int r = hexVal(s[1])*16 + hexVal(s[2]);
        int g = hexVal(s[3])*16 + hexVal(s[4]);
        int b = hexVal(s[5])*16 + hexVal(s[6]);
        return RGB(r,g,b);
    }
    return RGB(255,255,255);
}

static BehaviorType parseBehavior(const std::string &b) {
    if(b=="static") return BehaviorType::Static;
    if(b=="orbit") return BehaviorType::Orbit;
    if(b=="follow"||b=="followlag") return BehaviorType::FollowLag;
    return BehaviorType::Mirror;
}

void handleCommand(const std::string &line) {
    auto kv = parseSimpleJson(line);
    auto cmdIt = kv.find("cmd"); if(cmdIt==kv.end()) return;
    std::string cmd = cmdIt->second;
    printf("IPC command line: %s\n", line.c_str());
    if(cmd=="add") {
        SwarmCursor c; c.size=12; c.color=RGB(0,200,255);
        if(kv.count("color")) c.color = parseColor(kv["color"]);
        if(kv.count("behavior")) c.behavior = parseBehavior(kv["behavior"]);
        if(kv.count("offsetX")) c.offsetX = atof(kv["offsetX"].c_str());
        if(kv.count("offsetY")) c.offsetY = atof(kv["offsetY"].c_str());
        if(kv.count("radius")) c.radius = atof(kv["radius"].c_str());
        if(kv.count("speed")) c.speed = atof(kv["speed"].c_str());
        if(kv.count("x")) c.target.x = (LONG)atof(kv["x"].c_str());
        if(kv.count("y")) c.target.y = (LONG)atof(kv["y"].c_str());
        if(kv.count("lagMs")) c.lagMs = atof(kv["lagMs"].c_str());
    if(kv.count("size")) { int s = atoi(kv["size"].c_str()); if(s>2 && s<400) c.size = s; }
        if(c.behavior==BehaviorType::Static) c.pos = c.target;
        int id = gManager.addCursor(c);
    printf("Added cursor id=%d behavior=%d color=%06lX lagMs=%.1f radius=%.1f\n", id, (int)c.behavior, (unsigned long)c.color, c.lagMs, c.radius);
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"event\":\"added\",\"id\":%d,\"behavior\":%d}\n", id, (int)c.behavior);
        sendOut(buf);
    } else if(cmd=="remove") {
        if(kv.count("id")) {
            int id = atoi(kv["id"].c_str());
            bool ok = gManager.removeCursor(id);
            printf("Remove cursor id=%d result=%s\n", id, ok?"ok":"notfound");
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"event\":\"removed\",\"id\":%d,\"ok\":%s}\n", id, ok?"true":"false");
            sendOut(buf);
        }
    } else if(cmd=="set") {
    if(!kv.count("id")) return; 
    int id = atoi(kv["id"].c_str());
        std::lock_guard<std::mutex> lock(gManager.mtx);
        for(auto &c : gManager.cursors) if(c.id==id) {
            if(kv.count("behavior")) c.behavior = parseBehavior(kv["behavior"]);
            if(kv.count("offsetX")) c.offsetX = atof(kv["offsetX"].c_str());
            if(kv.count("offsetY")) c.offsetY = atof(kv["offsetY"].c_str());
            if(kv.count("radius")) c.radius = atof(kv["radius"].c_str());
            if(kv.count("speed")) c.speed = atof(kv["speed"].c_str());
            if(kv.count("x")) c.target.x = (LONG)atof(kv["x"].c_str());
            if(kv.count("y")) c.target.y = (LONG)atof(kv["y"].c_str());
            if(kv.count("lagMs")) c.lagMs = atof(kv["lagMs"].c_str());
            if(kv.count("color")) c.color = parseColor(kv["color"]);
            if(kv.count("size")) { int s = atoi(kv["size"].c_str()); if(s>2 && s<400) c.size = s; }
            printf("Updated cursor id=%d behavior=%d\n", id, (int)c.behavior);
            char buf[160];
            snprintf(buf, sizeof(buf), "{\"event\":\"updated\",\"id\":%d,\"behavior\":%d}\n", id, (int)c.behavior);
            sendOut(buf);
        }
    } else if(cmd=="debug") {
        if(kv.count("mode")) {
            std::string m = kv["mode"];
            if(m=="solidOn") {
                gSolidMode = true;
                if(gManager.overlayWnd) {
                    // Remove color key, set semi opaque alpha
                    SetLayeredWindowAttributes(gManager.overlayWnd, 0, (BYTE)200, LWA_ALPHA);
                    printf("Debug solid mode ON (alpha background).\n");
                }
            } else if(m=="solidOff") {
                gSolidMode = false;
                if(gManager.overlayWnd) {
                    SetLayeredWindowAttributes(gManager.overlayWnd, RGB(0,0,0), 0, LWA_COLORKEY);
                    printf("Debug solid mode OFF (color key transparency).\n");
                }
            } else if(m=="windowed") {
                SwitchToWindowed(gManager.overlayWnd);
            } else if(m=="overlay") {
                SwitchToOverlay(gManager.overlayWnd);
            }
        }
    } else if(cmd=="clear") {
        {
            std::lock_guard<std::mutex> lock(gManager.mtx);
            gManager.cursors.clear();
        }
        printf("All cursors cleared.\n");
        sendOut("{\"event\":\"cleared\"}\n");
    } else if(cmd=="list") {
        std::vector<SwarmCursor> copy; {
            std::lock_guard<std::mutex> lock(gManager.mtx); copy = gManager.cursors; }
        for(auto &c : copy) {
            char buf[256];
            snprintf(buf, sizeof(buf), "{\"event\":\"cursor\",\"id\":%d,\"behavior\":%d,\"x\":%ld,\"y\":%ld}\n", c.id, (int)c.behavior, c.pos.x, c.pos.y);
            sendOut(buf);
        }
        sendOut("{\"event\":\"listDone\"}\n");
    } else if(cmd=="exit") {
        printf("Exit command received. Shutting down...\n");
        sendOut("{\"event\":\"exiting\"}\n");
        gManager.running = false;
        if(gManager.overlayWnd) PostMessage(gManager.overlayWnd, WM_CLOSE, 0, 0);
    }
}

void PipeThread() {
    const wchar_t *pipeName = L"\\\\.\\pipe\\SwarmPipe";
    while(gManager.running) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
        if(hPipe==INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError()==ERROR_PIPE_CONNECTED);
        if(connected) {
            std::string buffer; buffer.reserve(512);
            char chunk[256]; DWORD read=0;
            while(gManager.running && ReadFile(hPipe, chunk, sizeof(chunk), &read, nullptr)) {
                if(read==0) break;
                for(DWORD i=0;i<read;i++) {
                    if(chunk[i]=='\n') { handleCommand(buffer); buffer.clear(); }
                    else buffer.push_back(chunk[i]);
                }
            }
            if(!buffer.empty()) handleCommand(buffer);
        }
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

void OutPipeThread() {
    const wchar_t *pipeName = L"\\\\.\\pipe\\SwarmPipeOut";
    while(gManager.running) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
        if(hPipe==INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError()==ERROR_PIPE_CONNECTED);
        if(connected) {
            {
                std::lock_guard<std::mutex> lock(gOutPipeMtx);
                gOutPipe = hPipe;
                gOutPipeReady = true;
            }
            sendOut("{\"event\":\"connected\"}\n");
            // Block until client disconnects or shutdown
            while(gManager.running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if(WaitForSingleObject(hPipe, 0)==WAIT_OBJECT_0) break; // unlikely
            }
        }
        {
            std::lock_guard<std::mutex> lock(gOutPipeMtx);
            if(gOutPipe==hPipe) {
                gOutPipeReady = false; gOutPipe = INVALID_HANDLE_VALUE;
            }
        }
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
    case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            if(gSolidMode) {
                // Solid dark background so user can see overlay area in debug
                HBRUSH bg = CreateSolidBrush(RGB(20,20,20));
                FillRect(hdc, &rc, bg);
                DeleteObject(bg);
            } else {
                // Transparent via color key (black)
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            // Draw translucent background by layering attributes (set elsewhere)
            std::vector<SwarmCursor> copy;
            {
                std::lock_guard<std::mutex> lock(gManager.mtx);
                copy = gManager.cursors;
            }
            for(const auto &c : copy) {
                HBRUSH b = CreateSolidBrush(c.color);
                HBRUSH old = (HBRUSH)SelectObject(hdc, b);
                int s = c.size;
                Ellipse(hdc, c.pos.x - s/2, c.pos.y - s/2, c.pos.x + s/2, c.pos.y + s/2);
                SelectObject(hdc, old);
                DeleteObject(b);
            }
            if(gShowHelp) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(230,230,230));
                const wchar_t *lines[] = {
                    L"Swarm Controls (focus window to use)",
                    L"D : Toggle solid background",
                    L"W : Toggle windowed/overlay",
                    L"O : Add orbit cursor",
                    L"F : Add follow cursor",
                    L"C : Clear cursors",
                    L"X : Exit",
                    L"H : Hide/show this help",
                    L"Digits 1-6 also trigger same actions",
                    L"(Global Ctrl+Alt hotkeys may have failed to register)"
                };
                int y=10; for(auto *ln: lines){ TextOutW(hdc, 10, y, ln, (int)wcslen(ln)); y+=18; }
            }
            static int paintCount = 0;
            if(paintCount < 60) {
                printf("WM_PAINT frame=%d cursors=%zu firstPos=(%ld,%ld)\n", paintCount, copy.size(), copy.empty()?0:copy[0].pos.x, copy.empty()?0:copy[0].pos.y);
                paintCount++;
            }
            EndPaint(hWnd, &ps);
        } return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_HOTKEY: {
            UINT id = (UINT)wParam;
            if(id==1) { // toggle solid debug background
                bool newVal = !gSolidMode.load();
                gSolidMode = newVal;
                if(gManager.overlayWnd) {
                    if(newVal) {
                        SetLayeredWindowAttributes(gManager.overlayWnd, 0, (BYTE)200, LWA_ALPHA);
                        printf("Hotkey: solid background ON\n");
                    } else {
                        SetLayeredWindowAttributes(gManager.overlayWnd, RGB(0,0,0), 0, LWA_COLORKEY);
                        printf("Hotkey: solid background OFF\n");
                    }
                }
            } else if(id==2) { // toggle windowed/overlay
                if(gWindowedMode) SwitchToOverlay(gManager.overlayWnd); else SwitchToWindowed(gManager.overlayWnd);
            } else if(id==3) { // add orbit cursor
                SwarmCursor c; c.behavior=BehaviorType::Orbit; c.radius=80; c.speed=1.0; c.color=RGB(255,140,0); c.size=14; gManager.addCursor(c); printf("Hotkey: added orbit cursor\n");
            } else if(id==4) { // add follow cursor
                SwarmCursor c; c.behavior=BehaviorType::FollowLag; c.lagMs=400; c.color=RGB(120,160,255); c.size=12; gManager.addCursor(c); printf("Hotkey: added follow cursor\n");
            } else if(id==5) { // clear
                {
                    std::lock_guard<std::mutex> lock(gManager.mtx);
                    gManager.cursors.clear();
                }
                printf("Hotkey: cleared cursors\n");
            } else if(id==6) { // exit
                printf("Hotkey: exiting\n");
                gManager.running=false;
                if(gManager.overlayWnd) PostMessage(gManager.overlayWnd, WM_CLOSE, 0,0);
            }
        } return 0;
        case WM_KEYDOWN: { // Fallback when global hotkeys fail; use keys directly while window focused
            int vk = (int)wParam;
            if(vk=='H') { gShowHelp = !gShowHelp; InvalidateRect(hWnd,nullptr,FALSE); printf("Help %s\n", gShowHelp?"shown":"hidden"); return 0; }
            auto act=[&](int code){
                switch(code){
                    case 'D': case '1': {
                        bool newVal=!gSolidMode.load(); gSolidMode=newVal; if(gManager.overlayWnd){ if(newVal) SetLayeredWindowAttributes(gManager.overlayWnd,0,(BYTE)200,LWA_ALPHA); else SetLayeredWindowAttributes(gManager.overlayWnd,RGB(0,0,0),0,LWA_COLORKEY);} printf("Key(solid %s)\n",newVal?"ON":"OFF"); } break;
                    case 'W': case '2': { if(gWindowedMode) SwitchToOverlay(gManager.overlayWnd); else SwitchToWindowed(gManager.overlayWnd); printf("Key(toggle window/overlay)\n"); } break;
                    case 'O': case '3': { SwarmCursor c; c.behavior=BehaviorType::Orbit; c.radius=80; c.speed=1.0; c.color=RGB(255,140,0); c.size=14; gManager.addCursor(c); printf("Key(orbit added)\n"); } break;
                    case 'F': case '4': { SwarmCursor c; c.behavior=BehaviorType::FollowLag; c.lagMs=400; c.color=RGB(120,160,255); c.size=12; gManager.addCursor(c); printf("Key(follow added)\n"); } break;
                    case 'C': case '5': { { std::lock_guard<std::mutex> lock(gManager.mtx); gManager.cursors.clear(); } printf("Key(cleared)\n"); } break;
                    case 'X': case '6': { printf("Key(exit)\n"); gManager.running=false; if(gManager.overlayWnd) PostMessage(gManager.overlayWnd,WM_CLOSE,0,0); } break;
                }
            };
            act(vk);
        } return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

HWND CreateOverlayWindow(HINSTANCE hInst) {
    const wchar_t CLASS_NAME[] = L"SwarmOverlayClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"SwarmOverlay", WS_POPUP,
        0,0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, hInst, nullptr);

    if(!hWnd) return nullptr;

    // Use color key: black will be fully transparent, so we draw colored shapes on black background
    if(!SetLayeredWindowAttributes(hWnd, RGB(0,0,0), 0, LWA_COLORKEY)) {
        printf("SetLayeredWindowAttributes failed: %lu\n", GetLastError());
    }
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    return hWnd;
}

void UpdateThread() {
    auto last = std::chrono::high_resolution_clock::now();
    while(gManager.running) {
        auto now = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(now-last).count();
        last = now;
        POINT p; GetCursorPos(&p);
        // If we are in windowed debug mode, translate screen coords to client coords
        if(gWindowedMode && gManager.overlayWnd) {
            POINT clientP = p;
            if(ScreenToClient(gManager.overlayWnd, &clientP)) {
                p = clientP;
            }
        }
        gManager.updateAll(dt, p);
        if(gManager.overlayWnd) InvalidateRect(gManager.overlayWnd, nullptr, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    printf("Swarm starting...\n");
    SetProcessDPIAware();
    // seed with a couple of cursors
    for(int i=0;i<3;i++) {
        SwarmCursor c; c.behavior=BehaviorType::Mirror; c.offsetX=i*18; c.offsetY=(i%2)*18; c.color = RGB(40+i*60, 200 - i*40, 120 + i*40); c.size=10 + i*2; gManager.addCursor(c);
    }
    SwarmCursor orbit; orbit.behavior=BehaviorType::Orbit; orbit.radius=90; orbit.speed=1; orbit.color=RGB(255,120,30); orbit.size=14; gManager.addCursor(orbit);
    SwarmCursor lag; lag.behavior=BehaviorType::FollowLag; lag.lagMs=300; lag.color=RGB(150,150,255); gManager.addCursor(lag);

    gManager.overlayWnd = CreateOverlayWindow(hInst);
    if(!gManager.overlayWnd) { printf("Failed to create overlay window.\n"); return 1; }
    printf("Overlay created HWND=%p\n", (void*)gManager.overlayWnd);

    // Force initial visibility: windowed + solid debug background + large static reference cursor.
    // This helps first-time users who might otherwise not notice transparent overlay.
    SwitchToWindowed(gManager.overlayWnd);
    gSolidMode = true;
    // Large magenta static cursor in middle of window for visibility (id will auto-assign)
    RECT wr; GetClientRect(gManager.overlayWnd, &wr);
    SwarmCursor big; big.behavior = BehaviorType::Static; big.target.x = (wr.right-wr.left)/2; big.target.y = (wr.bottom-wr.top)/2; big.pos = big.target; big.size = 100; big.color = RGB(255,0,255); gManager.addCursor(big);
    printf("Startup debug: windowed+solid mode enabled, large static cursor added. Use hotkeys or pipe debug commands to switch back to overlay.\n");

    // Register global hotkeys bound to thread (no window handle) and also attempt with window handle if first fails
    int hkOk=0; bool anyFail=false;
    auto tryReg=[&](int id, char ch){ if(RegisterHotKey(nullptr,id,MOD_CONTROL|MOD_ALT,ch)) { hkOk++; return true;} anyFail=true; return false; };
    tryReg(1,'D'); tryReg(2,'W'); tryReg(3,'O'); tryReg(4,'F'); tryReg(5,'C'); tryReg(6,'X');
    if(anyFail && gManager.overlayWnd) {
        // second attempt with window handle
        auto tryRegWnd=[&](int id,char ch){ if(RegisterHotKey(gManager.overlayWnd,id,MOD_CONTROL|MOD_ALT,ch)) hkOk++; };
        tryRegWnd(1,'D'); tryRegWnd(2,'W'); tryRegWnd(3,'O'); tryRegWnd(4,'F'); tryRegWnd(5,'C'); tryRegWnd(6,'X');
    }
    if(hkOk>0) printf("Hotkeys registered (%d). Ctrl+Alt+D/W/O/F/C/X and in-window keys + digits active. Press H to hide help.\n", hkOk);
    else printf("No global hotkeys registered. Use focused window keys (D/W/O/F/C/X or digits 1-6).\n");
    // Ensure focus so keydown fallback works immediately
    SetForegroundWindow(gManager.overlayWnd);
    SetFocus(gManager.overlayWnd);

    // Load config file (line-delimited JSON commands) if present
    const char *cfgName = "swarm_config.jsonl";
    if(std::filesystem::exists(cfgName)) {
        std::ifstream in(cfgName, std::ios::in);
        if(in) {
            std::string line; int count=0;
            while(std::getline(in, line)) {
                if(line.empty()) continue;
                if(line[0]=='#') continue;
                handleCommand(line);
                count++;
            }
            printf("Loaded %d config lines from %s\n", count, cfgName);
        }
    }

    std::thread updater(UpdateThread);
    std::thread pipeServer(PipeThread);
    std::thread outPipe(OutPipeThread);

    MSG msg;
    while(GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gManager.running = false;
    updater.join();
    pipeServer.join();
    outPipe.join();
    return 0;
}

// Provide ANSI WinMain for toolchains expecting WinMain symbol
extern "C" int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    return wWinMain(hInst, hPrev, nullptr, nCmdShow);
}
