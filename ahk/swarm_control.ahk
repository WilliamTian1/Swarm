; Swarm control script (prototype)
; Demonstrates hotkeys to toggle overlay and (future) spawn scripted behaviors

#SingleInstance force
#NoEnv
SendMode Input
SetWorkingDir %A_ScriptDir%

; Placeholder: would communicate with C++ core via WM_COPYDATA or shared memory

; Hotkey: Ctrl+Alt+S to show message placeholder
^!s::
    TrayTip, Swarm, (Prototype) Would signal core to add a wandering cursor., 5, 1
return

; Hotkey: Ctrl+Alt+X to exit all (would send shutdown)
^!x::
    TrayTip, Swarm, (Prototype) Requesting shutdown., 5, 17
    ExitApp
return
