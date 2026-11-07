@echo off
rem Puts the MagicMouseNative driver on the Magic Mouse (asks for administrator rights),
rem then starts the app.
"%~dp0Driver\mmdriver.exe" install
start "" "%~dp0MagicMouseNative.exe"
