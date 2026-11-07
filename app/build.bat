@echo off
rem Run from an "x64 Native Tools Command Prompt for VS".
where cl >nul 2>nul || (echo cl.exe not found - open the x64 Native Tools Command Prompt for VS first. & exit /b 1)
cl /nologo /EHsc /O2 /W4 /std:c++17 /DUNICODE /D_UNICODE MagicMouseNative.cpp /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED && echo. && echo Built MagicMouseNative.exe - run it; it lives in the tray.
