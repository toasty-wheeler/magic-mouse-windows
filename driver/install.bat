@echo off
rem Installs out\mmfilter on the Magic Mouse. Run as administrator after build.bat.
rem Needs test signing: Secure Boot off, "bcdedit /set testsigning on", reboot.
setlocal
net session >nul 2>nul || (echo Run this from an elevated [administrator] prompt. & exit /b 1)
bcdedit /enum {current} | findstr /i /r "testsigning.*Yes" >nul || (echo Test signing is off: turn Secure Boot off, run "bcdedit /set testsigning on" and reboot. & exit /b 1)
powershell -NoProfile -Command "try { if (Confirm-SecureBootUEFI) { exit 1 } } catch {}" || (echo Secure Boot is on, so Windows ignores test signing. Turn it off in the UEFI setup. & exit /b 1)

cd /d "%~dp0out" || exit /b 1
if not exist mmfilter.cat (echo out\mmfilter.cat missing - run build.bat first. & exit /b 1)

certutil -f -addstore Root mmfilter_test.cer >nul || exit /b 1
certutil -f -addstore TrustedPublisher mmfilter_test.cer >nul || exit /b 1
pnputil /add-driver mmfilter.inf || exit /b 1

rem Close the tray app first: its open handle on \\.\MagicMouseNative would keep the old
rem driver loaded, and the mouse would come back on the old code. /f is needed because
rem closing the app's window only hides it; then wait until it has really exited.
set APP=%~dp0..\app\MagicMouseNative.exe
set RESTART_APP=
tasklist /fi "imagename eq MagicMouseNative.exe" | find /i "MagicMouseNative.exe" >nul && set RESTART_APP=1
if defined RESTART_APP taskkill /f /im MagicMouseNative.exe >nul 2>nul
set WAITS=0
:wait_app
tasklist /fi "imagename eq MagicMouseNative.exe" | find /i "MagicMouseNative.exe" >nul || goto app_closed
set /a WAITS+=1
if %WAITS% geq 10 (echo MagicMouseNative.exe won't close - close it from the tray and run this again. & exit /b 1)
timeout /t 1 /nobreak >nul
goto wait_app
:app_closed

rem Force it onto the mouse; HidBth's generic match would otherwise compete on rank.
set DEVCON=%ProgramFiles(x86)%\Windows Kits\10\Tools\10.0.28000.0\x64\devcon.exe
"%DEVCON%" update mmfilter.inf "BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&030d"
"%DEVCON%" update mmfilter.inf "BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&0310" >nul 2>nul

rem Start the app again as the signed-in user (through Explorer), not elevated.
if defined RESTART_APP if exist "%APP%" explorer.exe "%APP%"

echo.
echo Done. The mouse's stack should now read: HidBth ^| mmfilter ^| BthEnum
