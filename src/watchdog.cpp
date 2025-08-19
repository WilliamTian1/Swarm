// Swarm Watchdog
// Monitors heartbeat file written by swarm overlay (swarm.exe) and restarts
// the overlay if the process dies or heartbeat becomes stale.
// Resume bullet: "Implemented self-healing watchdog (heartbeat + auto-restart <2s)."

#include <windows.h>
#include <string>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <iostream>

struct Config {
    std::string exePath = "swarm.exe";
    std::string heartbeatFile = "swarm_heartbeat.txt";
    std::string stopFile = "swarm_watchdog.stop";
    int pollIntervalMs = 1000;      // ms between checks
    int staleThresholdMs = 5000;    // consider stale if older than this
    int staleRetries = 2;           // consecutive stale detections before restart
};

static bool fileExists(const std::string &p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static long long readHeartbeatTs(const std::string &path) {
    std::ifstream in(path, std::ios::in);
    if(!in) return -1;
    std::string line; if(!std::getline(in,line)) return -1;
    std::stringstream ss(line); long long v=-1; ss>>v; return v;
}

static std::string nowStr() {
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[64];
    snprintf(buf,sizeof(buf),"%04d-%02d-%02d %02d:%02d:%02d.%03d",
        (int)st.wYear,(int)st.wMonth,(int)st.wDay,(int)st.wHour,(int)st.wMinute,(int)st.wSecond,(int)st.wMilliseconds);
    return buf;
}

int main(int argc,char *argv[]) {
    Config cfg;
    for(int i=1;i<argc;i++) {
        std::string a = argv[i];
        auto next=[&](std::string &dst){ if(i+1<argc) dst = argv[++i]; };
        if(a=="--exe") next(cfg.exePath);
        else if(a=="--heartbeat") next(cfg.heartbeatFile);
        else if(a=="--interval") { std::string v; next(v); cfg.pollIntervalMs = atoi(v.c_str()); }
        else if(a=="--staleMs") { std::string v; next(v); cfg.staleThresholdMs = atoi(v.c_str()); }
        else if(a=="--stopFile") next(cfg.stopFile);
        else if(a=="--retries") { std::string v; next(v); cfg.staleRetries = atoi(v.c_str()); }
        else if(a=="--help" || a=="-h") {
            std::cout << "Usage: swarm_watchdog.exe [--exe swarm.exe] [--heartbeat swarm_heartbeat.txt]\n"
                      << "       [--interval 1000] [--staleMs 5000] [--retries 2] [--stopFile swarm_watchdog.stop]\n";
            return 0;
        }
    }
    if(cfg.staleRetries < 1) cfg.staleRetries = 1;
    std::cout << "[watchdog] start exe=" << cfg.exePath << " heartbeat=" << cfg.heartbeatFile
              << " intervalMs=" << cfg.pollIntervalMs << " staleMs=" << cfg.staleThresholdMs
              << " retries=" << cfg.staleRetries << "\n";

    PROCESS_INFORMATION pi{}; STARTUPINFOA si{}; si.cb = sizeof(si);
    auto closeHandles=[&](){ if(pi.hProcess){ CloseHandle(pi.hProcess); pi.hProcess=nullptr;} if(pi.hThread){ CloseHandle(pi.hThread); pi.hThread=nullptr;} };
    auto launch=[&](){
        if(pi.hProcess) return; // already running
        std::string cmd = cfg.exePath; // mutable buffer for CreateProcess
        if(CreateProcessA(nullptr, cmd.data(), nullptr,nullptr,FALSE, CREATE_NO_WINDOW, nullptr,nullptr,&si,&pi)) {
            std::cout << "[watchdog] launched pid=" << pi.dwProcessId << " @" << nowStr() << "\n";
        } else {
            std::cout << "[watchdog] CreateProcess failed gle=" << GetLastError() << "\n";
        }
    };

    int staleCount = 0;
    while(true) {
        if(fileExists(cfg.stopFile)) { std::cout << "[watchdog] stop file -> exit\n"; break; }
        DWORD wait = WAIT_TIMEOUT;
        if(pi.hProcess) wait = WaitForSingleObject(pi.hProcess, 0);
        if(!pi.hProcess || wait == WAIT_OBJECT_0) {
            if(wait == WAIT_OBJECT_0) {
                std::cout << "[watchdog] overlay exited -> restart\n";
                closeHandles();
            }
            launch();
            staleCount = 0;
        }
        long long ts = readHeartbeatTs(cfg.heartbeatFile);
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        bool stale = (ts < 0) || (nowMs - ts > cfg.staleThresholdMs);
        if(stale) {
            staleCount++;
            std::cout << "[watchdog] stale heartbeat age=" << (ts<0? -1 : (nowMs - ts))
                      << "ms count=" << staleCount << " @" << nowStr() << "\n";
            if(staleCount >= cfg.staleRetries) {
                if(pi.hProcess) {
                    std::cout << "[watchdog] restarting overlay (stale)\n";
                    TerminateProcess(pi.hProcess, 0);
                    WaitForSingleObject(pi.hProcess, 1500);
                    closeHandles();
                }
                launch();
                staleCount = 0;
            }
        } else if(staleCount>0) {
            std::cout << "[watchdog] heartbeat recovered\n";
            staleCount = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.pollIntervalMs));
    }
    closeHandles();
    return 0;
}
