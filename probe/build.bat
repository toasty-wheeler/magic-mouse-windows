@echo off
rem Run from an "x64 Native Tools Command Prompt for VS".
where cl >nul 2>nul || (echo cl.exe not found - open the x64 Native Tools Command Prompt for VS first. & exit /b 1)
cl /nologo /EHsc /O2 /W4 mmprobe.cpp && echo. && echo Built mmprobe.exe - run it with the mouse connected.
