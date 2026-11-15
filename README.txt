REMOTE-SLEEP
Hold the "previous" button of the Kobo Remote for 1 second -> the Kobo goes to sleep.
Made for the Kobo Libra Colour on firmware 4.x (not firmware 5.x).

Status: built and logic-tested on a computer, NOT yet tried on a real Kobo + Kobo Remote.
Use the troubleshooting steps below if the first try does not work.


INSTALL
1. Connect the Kobo to your computer with USB and tap "Connect".
2. Open the KOBOeReader drive and show hidden files
   (Mac: Cmd + Shift + .   Windows: View > Hidden items).
3. Copy  install/KoboRoot.tgz  into the  .kobo  folder. Do not unpack or rename it.
4. Eject the Kobo. It shows "Updating" briefly and restarts.
5. Make sure the remote is paired as usual (Settings > Bluetooth), turn a page with it,
   then hold "previous" for about a second.

Wake the Kobo with its own power button, as usual.


SETTINGS
After the remote has connected once, a settings file appears on the Kobo drive:
  .adds/remote-sleep/config.txt
Open it with a plain text editor. Changes apply after a restart of the Kobo.

  hold_ms=1000        how long to hold, in milliseconds
  prev_key=...        key code(s) of the previous button
  grab=1              1 = a long press turns no page (short clicks are passed on
                          the moment you release the button)
                      0 = listen only; a long press also turns back a page first
  repeat_gap_ms=0     see troubleshooting
  device_name=        (part of) the remote's name, if you use other Bluetooth devices too
  debug=0             1 = write a log to .adds/remote-sleep/log.txt


TROUBLESHOOTING
Set debug=1 in config.txt, restart the Kobo, press each remote button once, then hold
"previous" for two seconds. Connect to the computer and open .adds/remote-sleep/log.txt.

- No log.txt / no "using ... as remote" line:
  the remote was not recognised. The log lists every input device with its name;
  put the remote's name in device_name=.
- You see "key <number> down/up" but nothing happens on a long press:
  the previous button has a different number. Put it in prev_key=.
- Holding the button shows many quick down/up pairs instead of one long "down":
  the remote repeats clicks. Set repeat_gap_ms=250. (The pages turned during the
  hold are not undone in that case.)
- Holding the button shows one down/up pair and nothing else:
  the remote does not tell the Kobo that the button is being held, and a long
  press cannot be detected. Send me the log and we pick another gesture.
- Page turning with the remote stopped working or feels off: set grab=0.

Set debug=0 again when done.


UNINSTALL
Copy  uninstall/KoboRoot.tgz  into the .kobo folder and eject. Afterwards you can
delete the .adds/remote-sleep folder.


NOTES
- Only works in Kobo's own reading app, not inside KOReader.
- With grab=1, holding "next" does not fast-flip pages; each click is one page.
- A Kobo firmware update can remove the tool; install it again if so.
- This is an unofficial modification of the device's system files. It is small and
  removable, but as with any Kobo mod, a factory reset is the fallback if something
  goes wrong (your Kobo-store books re-sync; sideloaded files should be backed up).
- src/ contains the source code.
