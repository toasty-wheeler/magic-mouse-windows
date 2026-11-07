@echo off
rem Run from an "x64 Native Tools Command Prompt for VS".
where cl >nul 2>nul || (echo cl.exe not found - open the x64 Native Tools Command Prompt for VS first. & exit /b 1)
cd /d "%~dp0"
cl /nologo /EHsc /O2 /W4 /std:c++17 /DUNICODE /D_UNICODE mmdriver.cpp /link setupapi.lib newdev.lib crypt32.lib advapi32.lib shell32.lib || exit /b 1
echo. & echo Built mmdriver.exe
