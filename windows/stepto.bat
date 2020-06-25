if exist Debug\hidtest.exe goto debug
set HIDTEST=hidtest.exe
goto check
:debug
set HIDTEST=Debug\hidtest.exe
:check
if .%1==. goto default
set POS=%1
goto endif
:default
set POS=8000
:endif
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
