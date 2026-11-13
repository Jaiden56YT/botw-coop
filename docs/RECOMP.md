# BotW co-op on Savage Wind Recomp

[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) runs Breath of the Wild 1.9.0 as a PC program, built from your own game files. The co-op mod runs in it too, and a recomp player plays together with Ryujinx and Switch players through the same relay.

**Where this stands (2026-11-11):**

- **Mac (Apple silicon): works.** Most of the mod's testing is done there: up to two recomp copies with a Ryujinx player and a real Switch.
- **Windows: never run.** The changed Savage Wind builds on Windows (checked on GitHub's Windows machine), and the mod's own code generates and compiles for a Windows PC, but nobody has built the game with the mod in it or started it there yet. If you try it, you are the first: follow [If it doesn't work](#if-it-doesnt-work) so the cause can be found.
- **Linux: never run**, and nothing was checked for it beyond what Mac and Linux share.

It's not a download-and-play install. The official Savage Wind download can't load code mods, so Savage Wind is built on your PC from a changed source, with the mod compiled into the game code. One script does all of it (the build tools, your game files, the mod, the build), but it needs about 20 GB of disk and half an hour the first time. If that isn't for you, play in Ryujinx: see [PLAYING.md](PLAYING.md).

## What you need

- Your own Breath of the Wild with the 1.9.0 update, **extracted** the way Savage Wind wants it: an `exefs` folder and a `romfs` folder with the update merged in. Savage Wind's README ("What you need") describes them. If the official Savage Wind already runs your game, you have them.
- [Git](https://git-scm.com/downloads) and Python 3.11 or newer.
- This mod's release package for your computer (the same one Ryujinx players use).
- Windows: Windows 10 or later, a graphics card with Vulkan 1.3.

## Windows

1. Get the Savage Wind source that loads code mods. In a Command Prompt:

   ```bat
   git clone -b coop-mods https://github.com/Jaiden56YT/SavageWindRecomp.git
   cd SavageWindRecomp
   ```

2. Install everything with your base game and your 1.9.0 update:

   ```bat
   install-coop.cmd "C:\path\to\base game.nsp" "C:\path\to\1.9.0 update.nsp"
   ```

   This one command:
   - installs the build tools if they aren't there yet (MSYS2 and Python 3.12; it takes a while and Windows may ask for permission);
   - downloads the newest co-op mod release from this mod's GitHub page;
   - reads your dump with your console keys (found by itself if Ryujinx on this PC has them; otherwise put your `prod.keys` in `C:\Users\<you>\.switch\`) and writes the game's files into `SavageWindRecomp\extracted` (about 15 GB, several minutes);
   - builds the mod's game files, puts the mod in, and builds Savage Wind with it (15 to 20 minutes the first time).

3. Open the settings file it names (`coop\romfs\botw-coop\config.ini`) and set `name`, `server` and `password`, as in [PLAYING.md](PLAYING.md).

4. Start `play-coop.cmd` in the `SavageWindRecomp` folder and load a save. A banner says "Co-op: connected".

To update to a newer co-op release, run `install-coop.cmd` again (without the two files: the game's files are already there).

## Mac (Apple silicon)

The build tools first (once): `brew install sdl3 ffmpeg molten-vk vulkan-loader vulkan-headers shaderc`, and a Python environment in `tools/venv` with `lz4 capstone unicorn` (`python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn`). Then:

```bash
git clone -b coop-mods https://github.com/Jaiden56YT/SavageWindRecomp.git
cd SavageWindRecomp
./install-coop.sh "/path/to/base game.nsp" "/path/to/1.9.0 update.nsp"
./play-coop.sh
```

## After an update of the mod

The mod's code is part of the built game, so every new version of the mod needs a rebuild: run `install-coop.cmd` (Mac: `./install-coop.sh`) again. It downloads the newest release and builds again (a few minutes; only what changed is built). Everyone who plays together needs the same version of the mod.

## What's different from Ryujinx

- **Saves.** Savage Wind keeps its own saves (the `save` folder). The mod's separate co-op copy lives inside them, as everywhere else. To bring a save over from Ryujinx or a Switch, see Savage Wind's `docs/save-converter.md`.
- **Far warps arrive.** The warp that often gets stuck in Ryujinx over several kilometres arrived every time in the recomp on a Mac. `warp_direct = true` in `config.ini` makes it one jump there.
- **Network.** Nothing to turn on: the mod uses the computer's own network.
- **The log** is in the window `play-coop` runs in. The mod's lines start with `[coop]`, the loader's with `[mods]`.

## If it doesn't work

| What happens | Likely cause | What to do |
|---|---|---|
| "Downloading the co-op mod failed" | No internet, or GitHub can't be reached. | Try again, or download `botw-coop-windows-x64.zip` from the mod's releases page yourself, unzip it, and run `setup.bat "<base>" "<update>" --recomp "<SavageWindRecomp folder>"` in it. |
| The setup says your console keys aren't there | No `prod.keys` where it looks. | Put the `prod.keys` you dumped from your own Switch in `C:\Users\<you>\.switch\` (the same file Ryujinx asks for). |
| The setup says the folder "isn't the Savage Wind source that loads code mods" | It's the official source or the official download. | Clone the `coop-mods` branch as in step 1. |
| The build stops with an error (`make gen` or `make`) | A build problem on your system. | Copy the last 30 lines and share them. |
| The game starts, but there's never a "Co-op:" banner | The mod isn't in the game code. | Look for `[mods] subsdk9 is a mod` in the log. If it isn't there, run `install-coop.cmd` again. |
| The log names a `main+...` line to "add to hooks.txt", other than `main+c61540` | The mod hooks a game function the game code wasn't built for. (The same message for `main+c61540` and the `sdk+...` lines is normal.) | Your package and `extracted\exefs\hooks.txt` are from different versions: run `setup --recomp` again, then `make gen` and `make -j`. |
| "Co-op: can't reach ..." | As in Ryujinx: the address, the relay, or a firewall. | See [PLAYING.md](PLAYING.md). |
| A crash, or the game closes | Unknown: this is new ground on Windows. | Share the whole log window's text, and what you were doing. |

To play without the mod again: delete `subsdk9`, `subsdk9.mod` and `hooks.txt` from `extracted\exefs`, run `make gen` and `make -j`, and start Savage Wind the normal way.
