MagicMouseNative (portable)
https://github.com/toasty-wheeler/magic-mouse-windows

Smooth scrolling, inertia, tap to click, gestures and battery level for the original
Apple Magic Mouse (2009, A1296) on Windows.

Before you start
  - Pair the Magic Mouse in Windows' Bluetooth settings.
  - Remove other Magic Mouse drivers (Magic Utilities, Apple Boot Camp).
  - The driver is test-signed, not signed by Microsoft. Windows only loads it with
    Secure Boot OFF (in your PC's UEFI/BIOS setup) and test signing ON. "Install
    driver" offers to turn test signing on; that takes a restart, and until then the
    mouse doesn't work. If BitLocker is on, have your recovery key ready before you
    change Secure Boot.

Use
  1. Run "Install driver.cmd" (asks for administrator rights).
  2. Restart if it says so.
  3. Run MagicMouseNative.exe. It lives in the tray; click the icon for the window.

Settings are saved in MagicMouseNative.ini next to the app, so the folder can live
anywhere. The driver itself is installed in Windows, though: before you delete this
folder, run "Remove driver.cmd".

Don't turn test signing off while the driver is installed: Windows would then block
it, and the mouse would stop working until you turn it back on or remove the driver.
