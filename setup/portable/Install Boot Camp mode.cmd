@echo off
rem Boot Camp mode: puts Apple's signed Boot Camp driver on the Magic Mouse instead of
rem MagicMouseNative's test-signed one (no test signing; Secure Boot can stay on) and turns on
rem Windows' full Bluetooth logging (asks for administrator rights). Apple's driver isn't
rem included: a copy already on this PC is used, or AppleWirelessMouse.inf next to
rem Driver\mmdriver.exe, or drag AppleWirelessMouse.inf onto this file.
"%~dp0Driver\mmdriver.exe" bootcamp %1
echo.
echo Restart Windows, then run MagicMouseNative.exe as administrator (right-click it).
pause
