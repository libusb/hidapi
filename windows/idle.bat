if exist Debug\hidtest.exe goto debug
set HIDTEST=hidtest.exe
goto check
:debug
set HIDTEST=Debug\hidtest.exe
:check
if .%1==. goto default
set MODE=%1
goto endif
:default
set MODE=0002
:endif
%HIDTEST% -v289d -p1000 -o00,0100,%MODE%,03E8,0000,0000,0000,0000,0000 -l15
