@echo off
rem Removes mmfilter from the driver store and the mouse (it falls back to plain HidBth).
rem Run as administrator.
setlocal
net session >nul 2>nul || (echo Run this from an elevated [administrator] prompt. & exit /b 1)

powershell -NoProfile -Command "$t = pnputil /enum-drivers | Out-String; $m = [regex]::Matches($t, 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+mmfilter\.inf'); if (-not $m.Count) { 'mmfilter is not in the driver store.' }; foreach ($x in $m) { pnputil /delete-driver $x.Groups[1].Value /uninstall /force }"

certutil -delstore Root "MagicMouseNative Test" >nul 2>nul
certutil -delstore TrustedPublisher "MagicMouseNative Test" >nul 2>nul
echo Done.
