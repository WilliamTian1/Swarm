# Swarm

Experimental Windows "multi-cursor" overlay system. Provides a transparent, click-through, always-on-top layered window that renders additional virtual cursors which can mirror the real mouse, remain static, or follow scripted behaviors.

## Resume-Style Highlights
- High-FPS (~60) transparent multi-cursor overlay (Win32 layered + custom polygon renderer)
- 25+ command automation bridge (add/remove/set/tweak/mouse/perf/save/load/reload/list/clear/...)
- Low-latency global input (Alt hotkeys + WH_KEYBOARD_LL hook)
- Hot-reload & persistence (config + state replay)
- Self-healing watchdog (heartbeat + auto-restart <2s typical downtime)

## Current Prototype
Implemented in C++ (`src/main.cpp`):
- Layered, transparent, full-screen overlay window (no taskbar icon)
- Multiple colored circular cursor indicators
- Positions mirror the system cursor with simple offset pattern
- ~60 FPS redraw loop

AutoHotkey script (`ahk/swarm_control.ahk`):
- Hotkeys (Ctrl+Alt+S) and (Ctrl+Alt+X) demonstrating future control surface
- Placeholder TrayTip notifications instead of IPC

## Build (CMake)
Requirements:
- Windows 10/11
- CMake >= 3.20
- Visual Studio Build Tools (MSVC) or MinGW-w64

Steps (PowerShell or Bash with VS Dev environment):
```
cmake -S . -B build
cmake --build build --config Release
```
Executable: `build/Release/SwarmOverlay.exe` (or `build/SwarmOverlay.exe` with MinGW single-config)

## Running
1. Launch `SwarmOverlay.exe` – colored dots should follow your cursor (offset pattern)
2. Optionally run `swarm_control.ahk` to enable prototype hotkeys

Exit by closing via Task Manager or adding an exit feature (planned). Currently overlay closes when standard WM_QUIT is posted (not yet bound to hotkey).

## Roadmap
Short Term:
- (DONE) Named Pipe IPC inbound commands
- (DONE) Behaviors: mirror, orbit, follow (lag), static
- (DONE) Line-delimited startup config `swarm_config.jsonl`
- (DONE) Debug modes: windowed / overlay / solid background
- (DONE) Outbound event pipe (basic)
- (DONE) Global hotkeys (Ctrl+Alt+D/W/O/F/C/X)

Medium Term:
- Shared memory ring buffer for high-frequency cursor command stream
- Lua or embedded scripting for behaviors (alternative to only AHK)
- Per-cursor trails, shapes, blended glow effects
- Performance optimization (Direct2D or DirectComposition instead of GDI)

Long Term / Stretch:
- Multi-machine broadcast (send cursor swarm over LAN via UDP)
- Recording & playback of cursor motion sets
- Plugin interface for custom behavior modules
- Installer & signed driver (if ever needed for deeper integration)

## Watchdog (High Availability)
Lightweight supervisor (`src/watchdog.cpp`) keeps the overlay alive.

Build (MSVC single-file):
```
cl /nologo /std:c++17 /O2 /EHsc src\watchdog.cpp /Fe:swarm_watchdog.exe /link user32.lib
```

Run:
```
./swarm_watchdog.exe --exe swarm.exe --heartbeat swarm_heartbeat.txt --interval 1000 --staleMs 5000
```

Stop gracefully (create sentinel; overlay continues running):
```
echo stop > swarm_watchdog.stop
```

Tune thresholds:
```
./swarm_watchdog.exe --staleMs 3000 --interval 750 --retries 2
```

Notes:
- Heartbeat file written once per second by overlay (first line = epoch ms).
- After N (retries) consecutive stale detections, overlay is terminated then relaunched.
- Simple polling design; could evolve to file change notifications, Windows service, or job object supervision.

## IPC Design Sketch
Inbound pipe: `\\.\\pipe\\SwarmPipe`
Outbound (events) pipe: `\\.\\pipe\\SwarmPipeOut`

Supported inbound commands (JSON object per line):
```
{"cmd":"add", "behavior":"orbit", "radius":80, "speed":1.2, "color":"#FF8833"}
{"cmd":"set", "id":3, "behavior":"follow", "lagMs":500}
{"cmd":"remove", "id":2}
{"cmd":"clear"}
{"cmd":"list"}
{"cmd":"exit"}
{"cmd":"debug", "mode":"windowed"}   # or overlay | solidOn | solidOff
{"cmd":"add", "behavior":"static", "x":500, "y":400, "color":"#22DD44"}
```

Events (lines) emitted on outbound pipe after you connect a reader:
```
{"event":"connected"}
{"event":"added","id":5,"behavior":2}
{"event":"updated","id":5,"behavior":1}
{"event":"removed","id":5,"ok":true}
{"event":"cleared"}
{"event":"cursor","id":1,"behavior":0,"x":123,"y":456}   # for each on list
{"event":"listDone"}
{"event":"exiting"}
```

Connect outbound pipe first (optional) so you get events immediately; then send inbound commands.

Example (PowerShell outbound read):
```
$p = New-Object IO.Pipes.NamedPipeClientStream '.' 'SwarmPipeOut' ([IO.Pipes.PipeDirection]::In)
$p.Connect(); $sr = New-Object IO.StreamReader $p; while($true){ $line=$sr.ReadLine(); if($line -eq $null){break}; Write-Host $line }
```

Then send commands inbound similarly (as previously documented).

Startup config file `swarm_config.jsonl`: each non-empty, non-# line is fed through the same command handler at launch.

## Hotkeys
Global (system-wide) hotkeys registered by the overlay:

Ctrl+Alt+D  Toggle solid debug background transparency
Ctrl+Alt+W  Toggle windowed <-> overlay mode
Ctrl+Alt+O  Add an orbit cursor (radius 80)
Ctrl+Alt+F  Add a follow-lag cursor (lag 400ms)
Ctrl+Alt+C  Clear all cursors
Ctrl+Alt+X  Exit overlay

Each hotkey prints a log line in the console. These are convenience controls while iterating.

The AutoHotkey script can `FileOpen("\\\\.\\pipe\\SwarmPipe", "w")` and `FileAppend` JSON lines. To receive events, open `SwarmPipeOut` for reading.

## License
TBD (choose MIT/Apache-2.0 recommended for openness).

## Disclaimer
Prototype quality. Not a security boundary. Overlay doesn't create true additional system cursors (Windows traditionally supports one). It visually simulates multiples.

