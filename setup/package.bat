@echo off
rem Builds dist\: the installer and the portable zip. Build the driver (..\driver\build.bat),
rem the app (..\app\build.bat) and mmdriver (build.bat) first. Needs Inno Setup's ISCC.
setlocal
cd /d "%~dp0"
set VERSION=1.0.0
for %%f in (..\app\MagicMouseNative.exe ..\driver\out\mmfilter.sys ..\driver\out\mmfilter.cat mmdriver.exe) do (
    if not exist %%f (echo %%f is missing - build it first. & exit /b 1)
)

set ISCC=ISCC.exe
where ISCC.exe >nul 2>nul || set ISCC="%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
%ISCC% /Q /DAppVersion=%VERSION% MagicMouseNative.iss || (echo Inno Setup failed. & exit /b 1)

set NAME=MagicMouseNative-%VERSION%-portable
set P=..\dist\%NAME%
if exist %P% rd /s /q %P%
mkdir %P%\Driver
copy /y ..\app\MagicMouseNative.exe %P%\ >nul
copy /y portable\* %P%\ >nul
copy /y mmdriver.exe %P%\Driver\ >nul
for %%f in (sys inf cat) do copy /y ..\driver\out\mmfilter.%%f %P%\Driver\ >nul
copy /y ..\driver\out\mmfilter_test.cer %P%\Driver\ >nul
if exist ..\dist\%NAME%.zip del ..\dist\%NAME%.zip
powershell -NoProfile -Command "Compress-Archive -Path '%P%' -DestinationPath '..\dist\%NAME%.zip'" || exit /b 1

echo.
echo dist\MagicMouseNative-%VERSION%-setup.exe
echo dist\%NAME%.zip
