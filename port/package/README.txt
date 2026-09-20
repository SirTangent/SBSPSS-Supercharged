SpongeBob SquarePants: SuperSponge - PC port test build
=======================================================

This folder is a complete, portable copy of the game.  Nothing is
installed; keep everything together and it runs from anywhere.

  sbsp.exe               the game (release build)
  sbsp-debug.exe         the same game with its internal checks switched on -
                         slower, and it stops with a message when something is
                         wrong.  Test sessions use this one.
  sbsp64.exe             (some builds) the same two as 64-bit programs - see
  sbsp64-debug.exe       "The 64-bit build".  SDL3.dll belongs to them.
  SDL3.dll
  run-test-session.cmd   runs one recorded test session (see "Test sessions")
  data\                  the game's data (about 170 MB)
  sbsp.ini               your settings - created on the first run
  saves\                 your memory card (card0.mcd)
  sessions\              created by run-test-session.cmd

Requirements: Windows 10 or 11 (64-bit is fine; sbsp.exe is 32-bit), a GPU
driver with Vulkan (any GPU from the last ten years: update the driver if the
window stays black), a gamepad or the keyboard.


Playing
-------
Double-click sbsp.exe.  A console window opens next to the game window; it
carries the game's log and can be ignored.  Close the game window to quit.

Keyboard (defaults; change them in sbsp.ini):

  PlayStation      Key                PlayStation      Key
  D-pad            Arrow keys         L1 / R1          Q / W
  Cross (X)        Z                  L2 / R2          E / R
  Circle (O)       X                  Start            Enter
  Square           A                  Select           Right Shift
  Triangle         S

  Alt+Enter        fullscreen on/off (borderless)

Any XInput or DualShock-style gamepad works as-is (D-pad, face buttons,
shoulders/triggers, Start/Back) including vibration.

The game pauses by itself while another window has the focus and carries on
when you come back.


Settings: sbsp.ini
------------------
Written beside sbsp.exe on the first run, with every option and its
default and one comment line each.  Edit with Notepad and restart the
game.  The options:

  window=1024x768            window size, or window=fullscreen
  scale=fit                  fit (4:3 with black bars), integer (whole
                             multiples of the original 256 lines), stretch
  vsync=1                    0 = no vsync
  audio_device=              part of a playback device's name; empty = default
  audio_buffer_frames=0      e.g. 512 or 1024 if the sound crackles; 0 = auto
  volume=100                 0-100
  key_up=Up ... key_r2=R     the keyboard bindings (key names as Windows
                             shows them: Space, Return, Right Shift, F1,
                             Keypad 0, ...)
  pad_deadzone=15            analog stick dead zone, percent
  rumble=1                   0 = never vibrate
  pause_on_focus_loss=1      0 = keep running in the background
  language=english           text language - NOTE: only English text exists in
                             the game data, so the other choices (swedish,
                             dutch, italian, german) show English too
  data_dir=                  where the data is; empty = the data\ folder here
  save_dir=                  where card0.mcd goes; empty = the saves\ folder here

The same options work on the command line, e.g.
  sbsp.exe --window fullscreen --scale integer --volume 50 --set rumble=0
and "sbsp.exe --help" lists everything.


Test sessions
-------------
Run run-test-session.cmd (double-click it) instead of the exe.  It starts
sbsp-debug.exe, records everything you press, keeps the logs, and copies
your memory card before and after.  Play as you normally would; save in the
game whenever you like - the card in saves\ is yours and persists between
sessions, so a long play-through can be split over several sessions.

When you close the game the script prints the exit code and the summary
line, and names the folder it made under sessions\.  Zip that folder and
send it together with a few words on what you did and anything that looked
wrong (a stuck screen, a missing sound, a wrong colour - anything).

Exit codes: 0 the game closed normally; 10 an internal check failed (the
game kept running - the message is in stderr.txt); 11 a crash; 12 the game
stopped responding; 13 a recording mismatch.

If sbsp-debug.exe shows a message box or the console shows a line starting
with [assert], that is exactly what we want to hear about: note what you
were doing at that moment.


The 64-bit build
----------------
If this folder has sbsp64.exe and sbsp64-debug.exe, they are the same game
built as a 64-bit program (64-bit Windows only).  They use the same data,
the same sbsp.ini and the same memory card as the 32-bit ones, so you can
switch between them in the middle of a play-through, and they should look,
sound and play exactly alike - any difference you notice between the two
is worth a note.  Keep SDL3.dll beside them; the 32-bit exes do not use it.

  run-test-session.cmd x64          a recorded session on sbsp64-debug.exe
  run-test-session.cmd x64 final    ... on sbsp64.exe

When you send a session, say which exe it was (session.pad also shows it:
its second line is "# abi ptr=8" for a 64-bit recording, "ptr=4" otherwise).


Trouble
-------
Black window / "Vulkan presenter unavailable" in the console: update the
GPU driver.  Very old GPUs without Vulkan cannot run this build.

"CdInit: no BIGLUMP.BIN": the data\ folder is not beside the exe - unpack
the whole zip, not just the exe.

Sound crackles: set audio_buffer_frames=1024 (or 2048) in sbsp.ini.

The game runs slow: it is a software-rendered PlayStation; a laptop on
battery-saver may not keep 60 frames per second.  Try scale=fit in a
smaller window, and vsync=0.

Wrong keys: the console prints "[input] key_xxx: unknown key name" if a
binding in sbsp.ini was not understood - the default stays in effect.
