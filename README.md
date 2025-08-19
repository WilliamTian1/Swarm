# Swarm

Experimental Windows "multi-cursor" overlay system. Provides a transparent, click-through, always-on-top layered window that renders additional virtual cursors which can mirror the real mouse, remain static, or follow scripted behaviors.

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
- IPC channel (WM_COPYDATA or Named Pipe) for AHK -> core commands
- Add behaviors: stationary, orbit, follow with delay, scripted path
- Configuration file (JSON / simple INI) for number/colors/behaviors
- Hotkey to toggle overlay visibility

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

## IPC Design Sketch
Initial approach: Named Pipe `\\.\\pipe\\SwarmPipe` with simple JSON messages, e.g.:
```
{"cmd":"add","behavior":"orbit","radius":40,"color":"#33FFAA"}
{"cmd":"remove","id":3}
{"cmd":"set","id":2,"behavior":"follow","lagMs":120}
```
The AutoHotkey script can `FileOpen("\\\\.\\pipe\\SwarmPipe", "w")` and `FileAppend` JSON lines.

## License
TBD (choose MIT/Apache-2.0 recommended for openness).

## Disclaimer
Prototype quality. Not a security boundary. Overlay doesn't create true additional system cursors (Windows traditionally supports one). It visually simulates multiples.

