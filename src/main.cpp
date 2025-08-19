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
#include <commdlg.h> // for file dialogs
#include <map>
#include <cctype>
#include <sstream>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <unordered_map>

/*
 Swarm prototype
 - Creates overlay window with additional visual cursors
 - Mirrors system cursor position to all swarm cursors (simple prototype)
 - Future: independent scripted behaviors, AHK integration via IPC or shared memory
*/

enum class BehaviorType { Mirror, Static, Orbit, FollowLag, Script };

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
    // Script integration
    std::string scriptPath;              // .ahk path when behavior==Script
    PROCESS_INFORMATION scriptPi{0};
    bool scriptProcessRunning {false};
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

// Draw a simple arrow (cursor-like) shape centered at (cx,cy)
// size ~= overall height of arrow
static void DrawCursorShape(HDC hdc, int cx, int cy, int size, COLORREF color) {
    // Base arrow coordinates (approx Windows arrow) in a 28px height box, origin at (0,0)
    // Points (x,y): tip at (0,0), down to (0,20), across to (6,14), to (11,28), (15,26), (9,13), (20,13), back to tip
    POINT base[] = {
        {0,0},{0,20},{6,14},{11,28},{15,26},{9,13},{20,13}
    };
    const int n = (int)(sizeof(base)/sizeof(base[0]));
    double scale = size / 28.0; // scale height
    // Determine width for centering (approx max x)
    int maxX = 20; int maxY = 28;
    int w = (int)(maxX * scale);
    int h = (int)(maxY * scale);
    int ox = cx - w/2; // shift so shape centered at (cx,cy)
    int oy = cy - h/2;
    POINT pts[16];
    for(int i=0;i<n;i++) {
        pts[i].x = (LONG)(ox + base[i].x * scale);
        pts[i].y = (LONG)(oy + base[i].y * scale);
    }
    HBRUSH b = CreateSolidBrush(color);
    HBRUSH old = (HBRUSH)SelectObject(hdc, b);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    Polygon(hdc, pts, n);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
    DeleteObject(b);
}

static SwarmManager gManager;
static std::atomic<bool> gSolidMode {false};
// (windowed/overlay mode flags removed in simplified always-overlay build)
static std::mutex gOutPipeMtx; // protects outbound pipe writes
static HANDLE gOutPipe = INVALID_HANDLE_VALUE; // outbound event stream
static std::atomic<bool> gOutPipeReady {false};
static bool gShowHelp = true; // draw help text overlay in windowed mode for user guidance
static HHOOK gLLHook = nullptr; // low-level keyboard hook for Alt combos
// Allow multiple simultaneous inbound clients to avoid ERROR_PIPE_BUSY.
static const int kMaxInboundInstances = 16; // maximum instances parameter passed to CreateNamedPipe
static const int kInboundListenerCount = 8; // number of concurrent listener threads (simultaneous pending ConnectNamedPipe)
// Performance metrics
static std::atomic<double> gAvgFrameMs {16.0};
static std::atomic<double> gLastFPS {60.0};
// Heartbeat control
static std::atomic<bool> gHeartbeatRunning {true};
static const char* kStateFile = "swarm_state.jsonl";
static const char* kConfigFile = "swarm_config.jsonl";
static const char* kHeartbeatFile = "swarm_heartbeat.txt";
static std::atomic<int> gApiCommandCount {0};
static std::string gAhkExePath = "AutoHotkey64.exe"; // configurable via setAhk command

// Forward declaration because script pipe reader feeds commands back
void handleCommand(const std::string &line);

// ---------------- Script per-cursor inbound pipe (script -> overlay) ---------------
struct ScriptPipeInfo {
    int id{0};
    std::wstring pipeNameW; // \\.\pipe\SwarmScript_<id>
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> running{true};
    std::thread th;
};
static std::mutex gScriptPipeMtx;
static std::unordered_map<int,std::unique_ptr<ScriptPipeInfo>> gScriptPipes; // by cursor id

static std::wstring MakeScriptPipeNameW(int id) {
    wchar_t buf[128]; swprintf(buf,128,L"\\\\.\\pipe\\SwarmScript_%d", id); return buf;
}

static void StopScriptPipe(int id) {
    std::unique_ptr<ScriptPipeInfo> owned;
    {
        std::lock_guard<std::mutex> lk(gScriptPipeMtx);
        auto it = gScriptPipes.find(id);
        if(it!=gScriptPipes.end()) { owned = std::move(it->second); gScriptPipes.erase(it); }
    }
    if(!owned) return;
    owned->running=false;
    if(owned->pipe!=INVALID_HANDLE_VALUE) {
        CancelIoEx(owned->pipe,nullptr);
        DisconnectNamedPipe(owned->pipe);
        CloseHandle(owned->pipe);
    }
    if(owned->th.joinable()) owned->th.detach();
}

static void ScriptPipeReader(int id, ScriptPipeInfo *spi) {
    BOOL connected = ConnectNamedPipe(spi->pipe,nullptr) ? TRUE : (GetLastError()==ERROR_PIPE_CONNECTED);
    if(!connected) {
        sendOut(std::string("{\"event\":\"scriptError\",\"id\":")+std::to_string(id)+",\"code\":\"connect\"}\n");
        return;
    }
    sendOut(std::string("{\"event\":\"scriptPipeConnected\",\"id\":")+std::to_string(id)+"}\n");
    std::string buf; buf.reserve(256);
    char chunk[128]; DWORD r=0;
    while(spi->running && ReadFile(spi->pipe, chunk, sizeof(chunk), &r, nullptr)) {
        if(r==0) break;
        for(DWORD i=0;i<r;i++) {
            char c = chunk[i];
            if(c=='\n' || c=='\r') {
                if(!buf.empty()) {
                    std::string line = buf; buf.clear();
                    std::istringstream iss(line); std::string cmd; iss>>cmd;
                    if(cmd=="pos") {
                        double x,y; if(iss>>x>>y) {
                            std::lock_guard<std::mutex> lk(gManager.mtx);
                            for(auto &c2: gManager.cursors) if(c2.id==id) { c2.pos.x=(LONG)x; c2.pos.y=(LONG)y; c2.target=c2.pos; }
                        }
                    } else if(cmd=="color") {
                        std::string col; if(iss>>col && col.size()==7 && col[0]=='#') {
                            auto hx=[&](char ch){ if(ch>='0'&&ch<='9') return ch-'0'; if(ch>='a'&&ch<='f') return 10+ch-'a'; if(ch>='A'&&ch<='F') return 10+ch-'A'; return 0; };
                            int r2=hx(col[1])*16+hx(col[2]); int g2=hx(col[3])*16+hx(col[4]); int b2=hx(col[5])*16+hx(col[6]);
                            std::lock_guard<std::mutex> lk(gManager.mtx);
                            for(auto &c2: gManager.cursors) if(c2.id==id) c2.color=RGB(r2,g2,b2);
                        }
                    } else if(cmd=="remove") {
                        StopScriptPipe(id);
                        std::string rm = std::string("{\"cmd\":\"remove\",\"id\":")+std::to_string(id)+"}";
                        handleCommand(rm);
                    } else if(cmd=="log") {
                        std::string rest; std::getline(iss,rest); if(!rest.empty() && rest[0]==' ') rest.erase(0,1);
                        sendOut(std::string("{\"event\":\"scriptLog\",\"id\":")+std::to_string(id)+",\"msg\":\""+rest+"\"}\n");
                    }
                }
            } else if(buf.size()<1024) buf.push_back(c);
        }
    }
    sendOut(std::string("{\"event\":\"scriptExit\",\"id\":")+std::to_string(id)+"}\n");
}

static void StartScriptPipe(int id) {
    auto spi = std::make_unique<ScriptPipeInfo>(); spi->id=id; spi->pipeNameW = MakeScriptPipeNameW(id);
    spi->pipe = CreateNamedPipeW(spi->pipeNameW.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, 1, 512,512,0,nullptr);
    if(spi->pipe==INVALID_HANDLE_VALUE) {
        sendOut(std::string("{\"event\":\"scriptError\",\"id\":")+std::to_string(id)+",\"code\":\"createPipe\"}\n");
        return;
    }
    ScriptPipeInfo *raw = spi.get();
    spi->th = std::thread([id,raw]{ ScriptPipeReader(id, raw); });
    {
        std::lock_guard<std::mutex> lk(gScriptPipeMtx);
        gScriptPipes[id] = std::move(spi);
    }
}

// Forward declarations for new helpers
void SaveState();
void LoadState();
POINT GetCursorPosForId(int id, bool *ok);
void PerformMouseAction(POINT p, const std::string &action, int button);
void ReloadConfigIfChanged(bool force=false);

// Forward declarations
// (keyboard hook & overlay key handling removed)

void sendOut(const std::string &line) {
    std::lock_guard<std::mutex> lock(gOutPipeMtx);
    if(gOutPipeReady && gOutPipe!=INVALID_HANDLE_VALUE) {
        std::string data = line; if(data.empty() || data.back()!='\n') data.push_back('\n');
        DWORD written=0; WriteFile(gOutPipe, data.data(), (DWORD)data.size(), &written, nullptr);
    }
}

static void ExecuteHotChar(char ch) {
    switch(ch) {
        case 'D': {
            bool newVal = !gSolidMode.load();
            gSolidMode = newVal;
            if(gManager.overlayWnd) {
                if(newVal) {
                    SetLayeredWindowAttributes(gManager.overlayWnd, 0, (BYTE)200, LWA_ALPHA);
                    printf("Hotkey: solid background ON (via %c)\n", ch);
                } else {
                    SetLayeredWindowAttributes(gManager.overlayWnd, RGB(0,0,0), 0, LWA_COLORKEY);
                    printf("Hotkey: solid background OFF (via %c)\n", ch);
                }
            }
        } break;
        case 'O': {
            SwarmCursor c; c.behavior=BehaviorType::Orbit; c.radius=80; c.speed=1.0; c.color=RGB(255,140,0); c.size=14; gManager.addCursor(c); printf("Hotkey: added orbit cursor (via %c)\n", ch);
        } break;
        case 'F': {
            SwarmCursor c; c.behavior=BehaviorType::FollowLag; c.lagMs=400; c.color=RGB(120,160,255); c.size=12; gManager.addCursor(c); printf("Hotkey: added follow cursor (via %c)\n", ch);
        } break;
        case 'C': {
            std::lock_guard<std::mutex> lock(gManager.mtx); gManager.cursors.clear(); printf("Hotkey: cleared cursors (via %c)\n", ch);
        } break;
        case 'X': {
            printf("Hotkey: exiting (via %c)\n", ch); gManager.running=false; if(gManager.overlayWnd) PostMessage(gManager.overlayWnd, WM_CLOSE, 0,0);
        } break;
        case 'S': {
            // Shift+Alt+S => create new script template; else pick existing
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000)!=0;
            if(shift) {
                // Save dialog to create new script
                wchar_t fileBuf[512] = L"";
                OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = gManager.overlayWnd;
                ofn.lpstrFilter = L"AutoHotkey Script (*.ahk)\0*.ahk\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = fileBuf; ofn.nMaxFile = 512;
                ofn.lpstrDefExt = L"ahk";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if(GetSaveFileNameW(&ofn)) {
                    std::wstring ws(fileBuf); std::string path(ws.begin(), ws.end());
                    // Write template
                    std::ofstream out(path, std::ios::out|std::ios::trunc);
                    if(out) {
                        out << "; Swarm cursor script template\n";
                        out << "; Arg1 (if provided) = cursor id\n";
                        out << "#NoTrayIcon\n";
                        out << "#SingleInstance Force\n";
                        out << "; Example: simple loop (adjust later to send IPC)\n";
                        out << "Sleep, 1000\n";
                    }
                    // Launch notepad for editing
                    std::string cmd = "notepad \"" + path + "\"";
                    system(cmd.c_str());
                    // Add script cursor (will launch AHK when added via command)
                    std::string json = std::string("{\"cmd\":\"add\",\"behavior\":\"script\",\"script\":\"") + path + "\"}";
                    handleCommand(json);
                }
            } else {
                wchar_t fileBuf[512] = L"";
                OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = gManager.overlayWnd;
                ofn.lpstrFilter = L"AutoHotkey Script (*.ahk)\0*.ahk\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = fileBuf; ofn.nMaxFile = 512;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if(GetOpenFileNameW(&ofn)) {
                    std::wstring ws(fileBuf); std::string path(ws.begin(), ws.end());
                    std::string json = std::string("{\"cmd\":\"add\",\"behavior\":\"script\",\"script\":\"") + path + "\"}";
                    handleCommand(json);
                }
            }
        } break;
    }
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if(code==HC_ACTION) {
        if(wParam==WM_KEYDOWN || wParam==WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT*)lParam;
            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000)!=0; // either Alt key
            if(altDown) {
                switch(k->vkCode) {
                    case 'D': case 'O': case 'F': case 'C': case 'X': case 'S':
                        ExecuteHotChar((char)k->vkCode);
                        return 1; // swallow so plain key not delivered
                }
            }
        }
    }
    return CallNextHookEx(gLLHook, code, wParam, lParam);
}

// (windowed/overlay switching removed)

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
    if(b=="script") return BehaviorType::Script;
    return BehaviorType::Mirror;
}

static bool LaunchScriptProcess(SwarmCursor &c) {
    if(c.scriptPath.empty()) return false;
    StartScriptPipe(c.id);
    std::wstring pipeW = MakeScriptPipeNameW(c.id);
    std::string pipeName(pipeW.begin(), pipeW.end());
    std::string cmd = gAhkExePath + " \"" + c.scriptPath + "\" " + std::to_string(c.id) + " " + pipeName;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr,nullptr,FALSE, CREATE_NO_WINDOW, nullptr,nullptr,&si,&pi);
    if(!ok) {
        printf("Script launch failed id=%d gle=%lu path=%s\n", c.id, GetLastError(), c.scriptPath.c_str());
        sendOut(std::string("{\"event\":\"scriptError\",\"id\":")+std::to_string(c.id)+",\"code\":\"launchFail\"}\n");
        return false;
    }
    c.scriptPi = pi; c.scriptProcessRunning=true;
    printf("Script launched id=%d pid=%lu path=%s pipe=%s\n", c.id, pi.dwProcessId, c.scriptPath.c_str(), pipeName.c_str());
    sendOut(std::string("{\"event\":\"scriptLaunched\",\"id\":")+std::to_string(c.id)+"}\n");
    return true;
}

static void CleanupScriptProcess(SwarmCursor &c) {
    if(!c.scriptProcessRunning) return;
    DWORD res = WaitForSingleObject(c.scriptPi.hProcess, 0);
    if(res!=WAIT_OBJECT_0) {
        TerminateProcess(c.scriptPi.hProcess, 0);
        WaitForSingleObject(c.scriptPi.hProcess, 500);
    }
    CloseHandle(c.scriptPi.hThread);
    CloseHandle(c.scriptPi.hProcess);
    c.scriptProcessRunning=false;
}

void handleCommand(const std::string &line) {
    auto kv = parseSimpleJson(line);
    // New structured protocol prefers 'op' over legacy 'cmd'
    auto opIt = kv.find("op");
    if(opIt!=kv.end()) {
        std::string op = opIt->second; // e.g. "cursor/add"
        gApiCommandCount++;
        // Dispatch new ops
        if(op=="help") {
            const char *ops[] = {
                "cursor/add","cursor/update","cursor/remove","cursor/clear","cursor/list",
                "mouse/click","mouse/down","mouse/up","mouse/drag",
                "state/save","state/load","state/reload",
                "sys/exit","sys/perf","config/setAhk","debug/mode"
            };
            for(auto &o: ops) { sendOut(std::string("{\"event\":\"help\",\"op\":\"")+o+"\"}\n"); }
            sendOut("{\"event\":\"helpDone\"}\n");
            return;
        } else if(op=="cursor/add") {
            kv["cmd"] = "add"; // reuse legacy path
        } else if(op=="cursor/update") {
            kv["cmd"] = "set";
        } else if(op=="cursor/remove") { kv["cmd"] = "remove"; }
        else if(op=="cursor/clear") { kv["cmd"] = "clear"; }
        else if(op=="cursor/list") { kv["cmd"] = "list"; }
        else if(op=="mouse/click") { kv["cmd"] = "clickId"; }
        else if(op=="mouse/down") { kv["cmd"] = "downId"; }
        else if(op=="mouse/up") { kv["cmd"] = "upId"; }
        else if(op=="mouse/drag") { kv["cmd"] = "dragId"; }
        else if(op=="state/save") { kv["cmd"] = "save"; }
        else if(op=="state/load") { kv["cmd"] = "load"; }
        else if(op=="state/reload") { kv["cmd"] = "reload"; }
        else if(op=="sys/exit") { kv["cmd"] = "exit"; }
        else if(op=="sys/perf") { kv["cmd"] = "perf"; }
        else if(op=="config/setAhk") { kv["cmd"] = "setAhk"; }
        else if(op=="debug/mode") { kv["cmd"] = "debug"; }
        else if(op=="cursor/tweak") { kv["cmd"] = "tweak"; }
        else {
            sendOut(std::string("{\"event\":\"error\",\"msg\":\"unknown op ")+op+"\"}\n");
            return;
        }
    }
    auto cmdIt = kv.find("cmd"); if(cmdIt==kv.end()) return; // still nothing
    std::string cmd = cmdIt->second;
    printf("IPC command: %s (line=%s)\n", cmd.c_str(), line.c_str());
    gApiCommandCount++;
    if(cmd=="add") {
        SwarmCursor c; c.size=12; c.color=RGB(0,200,255);
        if(kv.count("id")) { c.id = atoi(kv["id"].c_str()); if(c.id>=gManager.nextId.load()) gManager.nextId = c.id+1; }
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
        if(kv.count("script")) c.scriptPath = kv["script"];
        if(c.behavior==BehaviorType::Static) c.pos = c.target;
        int id = gManager.addCursor(c);
	printf("Added cursor id=%d behavior=%d color=%06lX lagMs=%.1f radius=%.1f script=%s\n", id, (int)c.behavior, (unsigned long)c.color, c.lagMs, c.radius, c.scriptPath.c_str());
        if(c.behavior==BehaviorType::Script) {
            // launch process for this cursor
            std::lock_guard<std::mutex> lock(gManager.mtx);
            for(auto &rc : gManager.cursors) if(rc.id==id) LaunchScriptProcess(rc);
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"event\":\"added\",\"id\":%d,\"behavior\":%d}\n", id, (int)c.behavior);
        sendOut(buf);
    } else if(cmd=="setAhk") {
        if(kv.count("path")) { gAhkExePath = kv["path"]; printf("Set AHK path: %s\n", gAhkExePath.c_str()); sendOut(std::string("{\"event\":\"ahkPath\",\"path\":\"")+gAhkExePath+"\"}\n"); }
    } else if(cmd=="remove") {
        if(kv.count("id")) {
            int id = atoi(kv["id"].c_str());
            bool ok=false; {
                std::lock_guard<std::mutex> lock(gManager.mtx);
                for(auto &c : gManager.cursors) if(c.id==id && c.behavior==BehaviorType::Script) CleanupScriptProcess(c);
                StopScriptPipe(id);
                ok = gManager.removeCursor(id);
            }
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
            } else if(m=="windowed" || m=="overlay") {
                printf("Debug: windowed/overlay disabled (always overlay).\n");
            } else if(m=="topOff") {
                if(gManager.overlayWnd) {
                    LONG_PTR ex2 = GetWindowLongPtr(gManager.overlayWnd, GWL_EXSTYLE);
                    SetWindowLongPtr(gManager.overlayWnd, GWL_EXSTYLE, ex2 & ~WS_EX_TOPMOST);
                    SetWindowPos(gManager.overlayWnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOREDRAW);
                    printf("Debug: topmost OFF.\n");
                }
            } else if(m=="topOn") {
                if(gManager.overlayWnd) {
                    LONG_PTR ex2 = GetWindowLongPtr(gManager.overlayWnd, GWL_EXSTYLE);
                    SetWindowLongPtr(gManager.overlayWnd, GWL_EXSTYLE, ex2 | WS_EX_TOPMOST);
                    SetWindowPos(gManager.overlayWnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOREDRAW);
                    printf("Debug: topmost ON.\n");
                }
            } else if(m=="keysOn" || m=="keysOff" || m=="clickOn" || m=="clickOff" || m=="mouseOn" || m=="mouseOff") {
                printf("Debug: keys/mouse capture disabled (always overlay pass-through).\n");
            }
        }
    } else if(cmd=="clear") {
        {
            std::lock_guard<std::mutex> lock(gManager.mtx);
            for(auto &c : gManager.cursors) if(c.behavior==BehaviorType::Script) { CleanupScriptProcess(c); StopScriptPipe(c.id);}            
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
    } else if(cmd=="click" || cmd=="clickId" || cmd=="downId" || cmd=="upId" || cmd=="dragId") {
        int id = kv.count("id") ? atoi(kv["id"].c_str()) : 0;
        bool ok=false; POINT p = GetCursorPosForId(id, &ok);
        if(!ok) { printf("Mouse action: invalid id=%d\n", id); return; }
        int button = kv.count("button") ? atoi(kv["button"].c_str()) : 0; // 0=left,1=right,2=middle
        if(cmd=="click") {
            PerformMouseAction(p, "down", button); PerformMouseAction(p, "up", button);
        } else if(cmd=="clickId") {
            PerformMouseAction(p, "down", button); PerformMouseAction(p, "up", button);
        } else if(cmd=="downId") {
            PerformMouseAction(p, "down", button);
        } else if(cmd=="upId") {
            PerformMouseAction(p, "up", button);
        } else if(cmd=="dragId") {
            // dragId requires dx & dy or tx & ty absolute target
            POINT target = p;
            if(kv.count("tx") && kv.count("ty")) { target.x = atol(kv["tx"].c_str()); target.y = atol(kv["ty"].c_str()); }
            else if(kv.count("dx") && kv.count("dy")) { target.x += atol(kv["dx"].c_str()); target.y += atol(kv["dy"].c_str()); }
            PerformMouseAction(p, "down", button);
            SetCursorPos(target.x, target.y);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            PerformMouseAction(target, "up", button);
        }
    } else if(cmd=="perf") {
        char buf[256];
        snprintf(buf,sizeof(buf),"{\"event\":\"perf\",\"fps\":%.1f,\"avgFrameMs\":%.3f,\"cursorCount\":%zu,\"apiCount\":%d}\n", gLastFPS.load(), gAvgFrameMs.load(), gManager.cursors.size(), gApiCommandCount.load());
        sendOut(buf);
    } else if(cmd=="save") {
        SaveState();
    } else if(cmd=="load") {
        LoadState();
    } else if(cmd=="reload") {
        ReloadConfigIfChanged(true);
    } else if(cmd=="tweak") {
        if(!kv.count("id")) return; int id = atoi(kv["id"].c_str());
        std::lock_guard<std::mutex> lock(gManager.mtx);
        for(auto &c : gManager.cursors) if(c.id==id) {
            if(kv.count("radius")) c.radius = atof(kv["radius"].c_str());
            if(kv.count("radiusDelta")) c.radius += atof(kv["radiusDelta"].c_str());
            if(kv.count("speed")) c.speed = atof(kv["speed"].c_str());
            if(kv.count("speedDelta")) c.speed += atof(kv["speedDelta"].c_str());
            if(kv.count("lagMs")) c.lagMs = atof(kv["lagMs"].c_str());
            if(kv.count("offsetX")) c.offsetX = atof(kv["offsetX"].c_str());
            if(kv.count("offsetY")) c.offsetY = atof(kv["offsetY"].c_str());
            if(kv.count("size")) { int s=atoi(kv["size"].c_str()); if(s>2 && s<400) c.size=s; }
            if(kv.count("color")) c.color = parseColor(kv["color"]);
            char buf2[200]; snprintf(buf2,sizeof(buf2),"{\"event\":\"tweaked\",\"id\":%d}\n", id); sendOut(buf2); break;
        }
    }
}

// Per-client reader handling inbound commands line-delimited.
static void InboundClientHandler(HANDLE hPipe) {
    std::string buffer; buffer.reserve(512);
    char chunk[256]; DWORD read=0;
    while(gManager.running && ReadFile(hPipe, chunk, sizeof(chunk), &read, nullptr)) {
        if(read==0) break;
        for(DWORD i=0;i<read;i++) {
            char c = chunk[i];
            if(c=='\n') { if(!buffer.empty()) handleCommand(buffer); buffer.clear(); }
            else if(buffer.size() < 4096) buffer.push_back(c); // limit line size
        }
    }
    if(!buffer.empty()) handleCommand(buffer);
    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

// Individual listener worker: waits for a client, hands off to handler synchronously, then loops.
static void InboundListenerWorker(int idx) {
    const wchar_t *pipeName = L"\\\\.\\pipe\\SwarmPipe";
    while(gManager.running) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            kMaxInboundInstances,
            4096, 4096, 0, nullptr);
        if(hPipe==INVALID_HANDLE_VALUE) {
            DWORD gle = GetLastError();
            if((idx==0)) printf("Inbound listener %d CreateNamedPipe failed gle=%lu\n", idx, gle);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError()==ERROR_PIPE_CONNECTED);
        if(connected) {
            // Handle in current thread (limits threads to listener count)
            InboundClientHandler(hPipe); // closes handle internally
        } else {
            CloseHandle(hPipe);
        }
    }
}

// Launch a pool of listener workers so multiple clients can connect simultaneously without transient ERROR_PIPE_BUSY.
void InboundListenerPool() {
    printf("Inbound pipe server pool starting with %d listeners (max instances %d).\n", kInboundListenerCount, kMaxInboundInstances);
    std::vector<std::thread> workers; workers.reserve(kInboundListenerCount);
    for(int i=0;i<kInboundListenerCount;i++) workers.emplace_back(InboundListenerWorker, i);
    while(gManager.running) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Shutdown: listeners will exit after gManager.running becomes false (may remain blocked until a client connects); we detach after a timeout to avoid hang.
    for(auto &t : workers) { if(t.joinable()) t.detach(); }
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
    case WM_NCHITTEST: return HTTRANSPARENT;
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
                DrawCursorShape(hdc, c.pos.x, c.pos.y, c.size, c.color);
            }
            if(gShowHelp) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(230,230,230));
                const wchar_t *lines[] = {
                    L"Swarm Alt Hotkeys:",
                    L"Alt+D solid bg toggle (debug)",
                    L"Alt+O add orbit cursor",
                    L"Alt+F add follow cursor",
                    L"Alt+C clear cursors",
                    L"Alt+S add script cursor (Shift=New)",
                    L"Alt+X exit",
                    L"H (focus) toggle help",
                    L"Always full-screen transparent overlay"
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
            UINT id = (UINT)wParam; char ch=0;
            if(id==1) ch='D'; else if(id==3) ch='O'; else if(id==4) ch='F'; else if(id==5) ch='C'; else if(id==6) ch='X'; else if(id==7) ch='S';
            if(ch) ExecuteHotChar(ch);
        } return 0;
    case WM_KEYDOWN: { int vk=(int)wParam; if(vk=='H'){ gShowHelp=!gShowHelp; InvalidateRect(hWnd,nullptr,FALSE); printf("Help %s\n", gShowHelp?"shown":"hidden"); } return 0; }
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
    WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, // don't start transparent to clicks until after mode decisions
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
    double emaMs = 16.0;
    while(gManager.running) {
        auto now = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(now-last).count();
        last = now;
        POINT p; GetCursorPos(&p);
    // (windowed mode removed; system cursor coords used directly)
        gManager.updateAll(dt, p);
        if(gManager.overlayWnd) InvalidateRect(gManager.overlayWnd, nullptr, FALSE);
        double frameMs = dt*1000.0;
        emaMs = emaMs*0.9 + frameMs*0.1;
        gAvgFrameMs = emaMs;
        if(emaMs>0.01) gLastFPS = 1000.0/emaMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
    }
}

static std::atomic<std::filesystem::file_time_type> gLastConfigTime;

void ReloadConfigIfChanged(bool force) {
    try {
        if(std::filesystem::exists(kConfigFile)) {
            auto mod = std::filesystem::last_write_time(kConfigFile);
            if(force || gLastConfigTime.load() != mod) {
                gLastConfigTime = mod;
                printf("Hot-reload: reloading %s\n", kConfigFile);
                std::ifstream in(kConfigFile, std::ios::in);
                if(in) {
                    std::string line; while(std::getline(in,line)) { if(line.empty()|| line[0]=='#') continue; handleCommand(line); }
                }
            }
        }
    } catch(...) { /* ignore */ }
}

void HotReloadThread() {
    while(gManager.running) {
        ReloadConfigIfChanged(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
}

void HeartbeatThread() {
    while(gManager.running && gHeartbeatRunning) {
        std::ofstream hb(kHeartbeatFile, std::ios::out|std::ios::trunc);
        if(hb) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            hb << ms << "\n" << gLastFPS.load() << "\n" << gManager.cursors.size() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

POINT GetCursorPosForId(int id, bool *ok) {
    *ok=false; POINT p{0,0};
    std::lock_guard<std::mutex> lock(gManager.mtx);
    for(auto &c: gManager.cursors) if(c.id==id) { p=c.pos; *ok=true; break; }
    return p;
}

void PerformMouseAction(POINT p, const std::string &action, int button) {
    INPUT inp{}; inp.type = INPUT_MOUSE;
    SetCursorPos(p.x, p.y);
    DWORD downFlag = MOUSEEVENTF_LEFTDOWN, upFlag = MOUSEEVENTF_LEFTUP;
    if(button==1) { downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag=MOUSEEVENTF_RIGHTUP; }
    else if(button==2) { downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag=MOUSEEVENTF_MIDDLEUP; }
    if(action=="down") inp.mi.dwFlags = downFlag; else if(action=="up") inp.mi.dwFlags = upFlag; else return;
    SendInput(1,&inp,sizeof(INPUT));
}

void SaveState() {
    std::lock_guard<std::mutex> lock(gManager.mtx);
    std::ofstream out(kStateFile, std::ios::out|std::ios::trunc);
    if(!out) { printf("SaveState: failed open %s\n", kStateFile); return; }
    for(auto &c: gManager.cursors) {
        std::string beh = (c.behavior==BehaviorType::Mirror?"mirror":(c.behavior==BehaviorType::Static?"static":(c.behavior==BehaviorType::Orbit?"orbit":(c.behavior==BehaviorType::FollowLag?"follow":"script"))));
        out << "{\"op\":\"cursor/add\",\"id\":" << c.id
            << ",\"behavior\":\"" << beh << "\""
            << ",\"offsetX\":" << c.offsetX << ",\"offsetY\":" << c.offsetY
            << ",\"radius\":" << c.radius << ",\"speed\":" << c.speed
            << ",\"lagMs\":" << c.lagMs << ",\"x\":" << c.target.x << ",\"y\":" << c.target.y
            << ",\"size\":" << c.size;
        if(c.behavior==BehaviorType::Script && !c.scriptPath.empty()) out << ",\"script\":\"" << c.scriptPath << "\"";
        out << "}\n";
    }
    printf("State saved (%zu cursors) to %s\n", gManager.cursors.size(), kStateFile);
}

void LoadState() {
    if(!std::filesystem::exists(kStateFile)) { printf("LoadState: file not found %s\n", kStateFile); return; }
    std::ifstream in(kStateFile, std::ios::in);
    if(!in) return;
    printf("Loading state from %s\n", kStateFile);
    std::string line; while(std::getline(in,line)) { if(line.empty()|| line[0]=='#') continue; handleCommand(line); }
    // Relaunch scripts (handleCommand already launches; this is defensive if future changes skip)
    {
        std::lock_guard<std::mutex> lock(gManager.mtx);
        for(auto &c : gManager.cursors) if(c.behavior==BehaviorType::Script && !c.scriptProcessRunning) LaunchScriptProcess(c);
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

    // Configure permanent transparent full-screen overlay (mouse pass-through)
    LONG_PTR st = GetWindowLongPtr(gManager.overlayWnd, GWL_STYLE);
    SetWindowLongPtr(gManager.overlayWnd, GWL_STYLE, (st & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    LONG_PTR ex = GetWindowLongPtr(gManager.overlayWnd, GWL_EXSTYLE);
    ex = (ex | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT) & ~WS_EX_APPWINDOW;
    SetWindowLongPtr(gManager.overlayWnd, GWL_EXSTYLE, ex);
    SetLayeredWindowAttributes(gManager.overlayWnd, RGB(0,0,0), 0, LWA_COLORKEY);
    SetWindowPos(gManager.overlayWnd, HWND_TOPMOST, 0,0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    printf("Startup: permanent transparent overlay active (Alt+D/O/F/C/X).\n");

    // Register global hotkeys and also set a low-level hook so Alt combos keep working after focus changes
    int hkOk=0; bool anyFail=false;
    auto tryAlt=[&](int id,char ch){ if(RegisterHotKey(nullptr,id,MOD_ALT,ch)) { hkOk++; return true;} anyFail=true; return false; };
    tryAlt(1,'D'); tryAlt(3,'O'); tryAlt(4,'F'); tryAlt(5,'C'); tryAlt(6,'X'); tryAlt(7,'S');
    if(anyFail && gManager.overlayWnd) {
        auto tryAltWnd=[&](int id,char ch){ if(RegisterHotKey(gManager.overlayWnd,id,MOD_ALT,ch)) hkOk++; };
        tryAltWnd(1,'D'); tryAltWnd(3,'O'); tryAltWnd(4,'F'); tryAltWnd(5,'C'); tryAltWnd(6,'X');
    }
    if(hkOk>0) printf("Hotkeys registered (%d). Alt+D/O/F/C/S/X (Shift+S new script). H toggles help. AHK=%s\n", hkOk, gAhkExePath.c_str());
    else printf("RegisterHotKey failed for all Alt combos, falling back to hook only.\n");

    gLLHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    if(gLLHook) printf("Low-level keyboard hook installed for Alt+D/O/F/C/S/X.\n");
    else printf("Failed to install low-level keyboard hook (gle=%lu).\n", GetLastError());
    // Do NOT force focus; user can Alt+Tab freely. (Focus only needed for fallback keys in windowed mode.)

    // Load config file (line-delimited JSON commands) if present
    ReloadConfigIfChanged(true);
    LoadState();

    std::thread updater(UpdateThread);
    std::thread pipeServer(InboundListenerPool);
    std::thread outPipe(OutPipeThread);
    std::thread hotReload(HotReloadThread);
    std::thread heartbeat(HeartbeatThread);

    MSG msg;
    while(GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gManager.running = false;
    updater.join();
    pipeServer.join();
    outPipe.join();
    hotReload.join();
    gHeartbeatRunning=false; heartbeat.join();
    if(gLLHook) { UnhookWindowsHookEx(gLLHook); gLLHook=nullptr; }
    return 0;
}

// Provide ANSI WinMain for toolchains expecting WinMain symbol
extern "C" int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    return wWinMain(hInst, hPrev, nullptr, nCmdShow);
}

// (Keyboard hook code removed; only Alt+ registered hotkeys remain.)
