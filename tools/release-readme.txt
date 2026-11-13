BotW co-op mod - version @VERSION@ - package for @SYSTEM@
=========================================================

Everyone who plays together needs the SAME version of this package.

What you need
  - Your own Breath of the Wild, dumped from your own Switch: the base game
    (.nsp or .xci) and the 1.9.0 update (.nsp). The mod only runs on 1.9.0.
  - Your console keys (prod.keys). If Ryujinx already runs your game, setup
    finds them by itself.
  - Python 3.11 or newer (https://www.python.org/downloads/). On Windows, tick
    "Add python.exe to PATH" in the installer.
  - To play on a computer: Ryujinx, with the game and the 1.9.0 update
    installed and working. Start it once before running setup.

1. Build and install (a few minutes the first time)

  Open a terminal (Windows: Command Prompt) in this folder and run:

    @START@

  It reads the files it needs from your dump, builds the mod's game files
  from them, and installs the mod into Ryujinx. It says what's missing if
  anything is. It never changes your dump.

  For a modded Switch instead (or as well), add:  --switch <folder>
  and read docs/SWITCH.md.

  For Savage Wind Recomp (the game as a PC program, built from source):
  you don't need this package. Clone the Savage Wind source that loads
  code mods and run its install-coop.cmd: see docs/RECOMP.md.

2. Settings

  Setup prints where config.ini is. Open it in a text editor and set:
    name     = the name your friends see
    server   = the address of whoever runs the relay (they tell you)
    password = the same word for everyone

  In Ryujinx: Options > Settings > Network > turn on Guest Internet Access.

3. Play

  ONE player runs the relay and leaves it open:

    @RELAY@

  It prints the addresses the others put in `server`. Players in other homes
  need a way to reach that computer: see "Playing over the internet" in
  docs/PLAYING.md (Tailscale is the easy way).

  Then everyone starts the game and loads a save. A message at the top of the
  screen says "Co-op: connected" and who's there.

How to play, what's shared and what to do when something goes wrong:
docs/PLAYING.md
