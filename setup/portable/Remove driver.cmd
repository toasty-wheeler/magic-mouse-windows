@echo off
rem Takes the MagicMouseNative driver off the Magic Mouse (asks for administrator rights);
rem the mouse goes back to Windows' own driver.
"%~dp0Driver\mmdriver.exe" uninstall
