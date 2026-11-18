MagicMouseNative (portable)
https://github.com/toasty-wheeler/magic-mouse-windows

Smooth scrolling, inertia, tap to click, gestures and battery level for the Apple Magic
Mouse on Windows (the 2009 A1296; the Magic Mouse 2 and USB-C Magic Mouse are recognised
too, untested). Optional: Apple keyboard remapping (Fn layer on Right Option, media keys,
Command/Option swap), in the tray menu.

Pair the Magic Mouse in Windows' Bluetooth settings first, then pick one of two ways:

A. MagicMouseNative's driver (test-signed)
  - Remove other Magic Mouse drivers (Magic Utilities, Apple Boot Camp).
  - Windows only loads this driver with Secure Boot OFF (in your PC's UEFI/BIOS setup)
    and test signing ON. "Install driver" offers to turn test signing on; that takes a
    restart, and until then the mouse doesn't work. If BitLocker is on, have your
    recovery key ready before you change Secure Boot.
  1. Run "Install driver.cmd" (asks for administrator rights).
  2. Restart if it says so.
  3. Run MagicMouseNative.exe. It lives in the tray; click the icon for the window.
  Don't turn test signing off while this driver is installed: Windows would block it, and
  the mouse would stop working until you turn it back on or remove the driver.

B. Boot Camp mode (Apple's signed driver; no test signing)
  - Apple's Boot Camp mouse driver isn't included. A copy already on this PC is used;
    otherwise drag AppleWirelessMouse.inf (from Apple's Boot Camp drivers, with its .sys
    and .cat next to it) onto "Install Boot Camp mode.cmd".
  - It turns on Windows' full Bluetooth logging, which the app reads the touches from.
    That setting covers all Bluetooth devices: while an administrator program records the
    log, it can see what Bluetooth devices send, keyboards included. MagicMouseNative keeps
    only the mouse's touches, in memory. "Remove driver.cmd" turns it back off.
  1. Run "Install Boot Camp mode.cmd" (asks for administrator rights).
  2. Restart Windows.
  3. Run MagicMouseNative.exe as administrator (right-click it).

Settings are saved in MagicMouseNative.ini next to the app, so the folder can live
anywhere. The drivers are installed in Windows, though: before you delete this folder,
run "Remove driver.cmd" (it also undoes Boot Camp mode).
