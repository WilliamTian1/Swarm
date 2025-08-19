#include <windows.h>
#include <string>
#include <iostream>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>

// Helper to write a full string (with trailing \n) to inbound pipe
bool sendCommand(const std::string &line) {
    HANDLE h = CreateFileW(L"\\\\.\\pipe\\SwarmPipe", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if(h==INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open inbound pipe. GLE=" << GetLastError() << "\n";
        return false;
    }
    std::string out = line;
    if(out.empty() || out.back()!='\n') out.push_back('\n');
    DWORD written=0; BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
    if(!ok) {
        std::cerr << "WriteFile failed GLE=" << GetLastError() << "\n";
        return false;
    }
    return true;
}


struct EventCollector {
    HANDLE h{INVALID_HANDLE_VALUE};
    std::atomic<bool> running{false};
    std::vector<std::string> lines; 
    std::mutex m;
    std::thread th;
    void start() {
        // Attempt to connect (wait up to ~2s)
        for(int i=0;i<40;i++) {
            h = CreateFileW(L"\\\\.\\pipe\\SwarmPipeOut", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if(h!=INVALID_HANDLE_VALUE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(h==INVALID_HANDLE_VALUE) {
            std::cerr << "Could not connect to outbound pipe (events will be lost).\n";
            return;
        }
        running = true;
        th = std::thread([this]{
            std::string buf; char tmp[256]; DWORD read=0;
            while(running) {
                if(ReadFile(h, tmp, sizeof(tmp), &read, nullptr) && read>0) {
                    for(DWORD i=0;i<read;i++) {
                        if(tmp[i]=='\n') { std::lock_guard<std::mutex> lock(m); lines.push_back(buf); buf.clear(); }
                        else buf.push_back(tmp[i]);
                    }
                } else {
                    if(GetLastError()==ERROR_BROKEN_PIPE) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }
        });
    }
    void stop() {
        running = false;
        if(h!=INVALID_HANDLE_VALUE) { CancelIoEx(h, nullptr); CloseHandle(h); }
        if(th.joinable()) th.join();
    }
};

int main() {
    std::cout << "SwarmPipeTest starting...\n";
    EventCollector collector; collector.start();
    // Wait for connected event to ensure subsequent add/list events are captured
    int waitMs = 0;
    while(waitMs < 1500) {
        {
            std::lock_guard<std::mutex> lock(collector.m);
            if(!collector.lines.empty()) break; // first line should be connected
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waitMs += 50;
    }
    // Batch commands
    std::vector<std::string> cmds = {
        "{\"cmd\":\"add\",\"behavior\":\"orbit\",\"radius\":140,\"speed\":0.9,\"color\":\"#FF7733\"}",
        "{\"cmd\":\"add\",\"behavior\":\"follow\",\"lagMs\":600,\"color\":\"#55AAFF\"}",
        "{\"cmd\":\"add\",\"behavior\":\"static\",\"x\":500,\"y\":360,\"color\":\"#22DD44\"}",
        "{\"cmd\":\"list\"}"
    };
    for(auto &c : cmds) {
        sendCommand(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    collector.stop();
    std::lock_guard<std::mutex> lock(collector.m);
    std::cout << "Collected " << collector.lines.size() << " events:\n";
    for(auto &e : collector.lines) std::cout << e << '\n';
    std::cout << "Test client done.\n";
    return 0;
}
