@echo off
rem Run from an "x64 Native Tools Command Prompt for VS". Needs WDK 10.0.28000 (KMDF 1.15).
rem Output goes to out\: mmfilter.sys, mmfilter.inf, mmfilter.cat (test-signed) and the test cert.
setlocal
where cl >nul 2>nul || (echo cl.exe not found - open the x64 Native Tools Command Prompt for VS first. & exit /b 1)

set KITS=%ProgramFiles(x86)%\Windows Kits\10
set WDKVER=10.0.28000.0
set KMDF=1.15
rem The WDK doesn't ship the shared Bluetooth headers (bthdef.h, bthioctl.h) or signtool;
rem they come from the 10.0.26100 SDK.
set SDKVER=10.0.26100.0
set SIGNTOOL=%KITS%\bin\%SDKVER%\x64\signtool.exe
set CERTNAME=MagicMouseNative Test
if not exist "%KITS%\Include\%WDKVER%\km\wdm.h" (echo WDK %WDKVER% not found. & exit /b 1)
if not exist "%KITS%\Include\%SDKVER%\shared\bthdef.h" (echo SDK %SDKVER% not found. & exit /b 1)
rem Keep the VS prompt's user-mode SDK/CRT headers out of the kernel build.
set INCLUDE=

if not exist out mkdir out

cl /nologo /c /kernel /W4 /wd4324 /O2 /GS /Gy /Zi /Zp8 /GF /GR- /Zc:wchar_t ^
   /D_AMD64_ /DAMD64 /D_WIN64 /DNTDDI_VERSION=0x0A000000 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=15 ^
   /I"%KITS%\Include\%WDKVER%\km" /I"%KITS%\Include\%WDKVER%\shared" /I"%KITS%\Include\%WDKVER%\km\crt" ^
   /I"%KITS%\Include\%SDKVER%\shared" ^
   /I"%KITS%\Include\wdf\kmdf\%KMDF%" ^
   /Foout\ /Fdout\ mmfilter.c || exit /b 1

link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry /NODEFAULTLIB /MACHINE:X64 ^
   /RELEASE /DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO /NXCOMPAT /DYNAMICBASE /MERGE:_TEXT=.text;_PAGE=PAGE ^
   /LIBPATH:"%KITS%\Lib\%WDKVER%\km\x64" /LIBPATH:"%KITS%\Lib\wdf\kmdf\x64\%KMDF%" ^
   /OUT:out\mmfilter.sys out\mmfilter.obj ^
   WdfDriverEntry.lib WdfLdr.lib ntoskrnl.lib hal.lib wmilib.lib wdmsec.lib ntstrsafe.lib BufferOverflowFastFailK.lib || exit /b 1

copy /y mmfilter.inf out\ >nul
"%KITS%\bin\%WDKVER%\x64\stampinf.exe" -f out\mmfilter.inf -d * -v * || exit /b 1

rem Test-signing certificate in CurrentUser\My, created once.
powershell -NoProfile -Command "$c = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=%CERTNAME%' | Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERTNAME%' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; Export-Certificate -Cert $c -FilePath out\mmfilter_test.cer | Out-Null" || exit /b 1

"%SIGNTOOL%" sign /q /fd SHA256 /s My /n "%CERTNAME%" out\mmfilter.sys || exit /b 1
"%KITS%\bin\%WDKVER%\x86\Inf2Cat.exe" /driver:out /os:10_X64 /uselocaltime || exit /b 1
"%SIGNTOOL%" sign /q /fd SHA256 /s My /n "%CERTNAME%" out\mmfilter.cat || exit /b 1

echo.
echo Built and test-signed out\mmfilter.sys - install with install.bat (admin, test signing on).
