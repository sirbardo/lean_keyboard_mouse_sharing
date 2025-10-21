@echo off
echo Building Keyboard/Mouse Switcher...

gcc -O3 -static -o sender.exe sender.cpp -lws2_32 -municode -mwindows -lstdc++

if %ERRORLEVEL% NEQ 0 (
    echo Failed to build sender.exe
    exit /b 1
)

gcc -O3 -static -o receiver.exe receiver.cpp -lws2_32 -luser32 -lstdc++
if %ERRORLEVEL% NEQ 0 (
    echo Failed to build receiver.exe
    exit /b 1
)

echo.
echo Build successful!
echo.
echo Files created:
echo   - sender.exe   (run on gaming PC)
echo   - receiver.exe (run on streaming PC)
echo.