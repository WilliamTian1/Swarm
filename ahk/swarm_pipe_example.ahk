; Example AutoHotkey script sending JSON commands to Swarm named pipe
#SingleInstance force
#NoEnv
SendMode Input
SetWorkingDir %A_ScriptDir%

PipeName := "\\.\pipe\SwarmPipe"

sendLine(line) {
    global PipeName
    h := FileOpen(PipeName, "w")
    if !IsObject(h) {
        TrayTip, Swarm, Could not open pipe., 5, 17
        return
    }
    h.Write(line "`n")
    h.Close()
}

^!a:: ; Add orbit cursor
sendLine('{"cmd":"add","behavior":"orbit","radius":70,"speed":2,"color":"#33FFAA"}')
return

^!f:: ; Add follow lag cursor
sendLine('{"cmd":"add","behavior":"follow","lagMs":500,"color":"#FF55AA"}')
return

^!m:: ; Add mirror cursor with offset
sendLine('{"cmd":"add","behavior":"mirror","offsetX":40,"offsetY":-40,"color":"#55AAFF"}')
return
