if exist Debug\hidtest.exe goto debug
set HIDTEST=hidtest.exe
goto check
:debug
set HIDTEST=Debug\hidtest.exe
:check
rem home gimbal
set POS=7e00
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem turn on light
set LIGHT=0001
%HIDTEST% -v289d -p1000 -o00,0001,%LIGHT%,0000,0000,0000,0000,0000,0000 -l15
rem wait .5 sec (simulate capture)
sleep 500
rem position to hot BB
set POS=7e80
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait .5 sec (simulate capture)
sleep 50
set LOOP=100
:loop
rem position to "short"
set POS=7f40
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait 1.0 sec (simulate capture)
sleep 1000
rem home gimbal
set POS=7e00
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait .5 sec (simulate capture)
sleep 500
rem position to "nominal"
set POS=7fa0
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait 1.0 sec (simulate capture)
sleep 1000
rem home gimbal
set POS=7e00
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait .5 sec (simulate capture)
sleep 500
rem home gimbal
set POS=7e00
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait .5 sec (simulate capture)
sleep 500
rem position to "tall"
set POS=8000
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait 1.0 sec (simulate capture)
sleep 1000
end
rem home gimbal
set POS=7e00
%HIDTEST% -v289d -p1000 -o00,0102,%POS%,%POS%,0000,0000,0000,0000,0000 -l15
rem wait .5 sec (simulate capture)
sleep 500
set /a LOOP=LOOP - 1
if /I %LOOP% GTR 0 goto loop
