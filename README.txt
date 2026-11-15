REMOTE-SLEEP (minimal)
Hold the "previous" button of the Kobo Remote for 1 second -> the Kobo goes to sleep.
For the Kobo Libra Colour on firmware 4.x. No settings, no files on the Kobo drive.

INSTALL (also replaces the earlier version)
1. Connect the Kobo with USB and show hidden files (Mac: Cmd + Shift + .).
2. Copy  install/KoboRoot.tgz  into the  .kobo  folder. Do not unpack or rename it.
3. Eject. The Kobo shows "Updating" briefly and restarts.
4. Optional: delete the old  .adds/remote-sleep  folder; it is no longer used.

UNINSTALL
Copy  uninstall/KoboRoot.tgz  into the .kobo folder and eject.

NOTES
- Short clicks are passed on when you release the button, so a long press turns no page.
- Works in Kobo's own reader, not inside KOReader.
- A firmware update can remove it; install again if so.
- Hold time and key codes are fixed in src/remote-sleep.c (HOLD_MS, PREV_KEYS).
