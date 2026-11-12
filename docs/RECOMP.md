# BotW co-op on Savage Wind Recomp

[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) runs Breath of the Wild 1.9.0 as a PC program, built from your own game files. The co-op mod runs in it too, and a recomp player plays together with Ryujinx and Switch players through the same relay.

**Where this stands (2026-11-11):**

- **Mac (Apple silicon): works.** Most of the mod's testing is done there: up to two recomp copies with a Ryujinx player and a real Switch.
- **Windows: never run.** The code that loads the mod compiles for Windows, and the mod's own code generates and compiles for a Windows PC, but nobody has started it there yet. If you try it, you are the first: follow [If it doesn't work](#if-it-doesnt-work) so the cause can be found.
- **Linux: never run**, and nothing was checked for it beyond what Mac and Linux share.

It is not an install. The official Savage Wind download can't load code mods, so you build Savage Wind from a changed source, with the mod compiled into the game code. That takes a programmer's toolchain, about 20 GB of disk and half an hour of building. If that isn't for you, play in Ryujinx: see [PLAYING.md](PLAYING.md).

## What you need

- Your own Breath of the Wild with the 1.9.0 update, **extracted** the way Savage Wind wants it: an `exefs` folder and a `romfs` folder with the update merged in. Savage Wind's README ("What you need") describes them. If the official Savage Wind already runs your game, you have them.
- [Git](https://git-scm.com/downloads) and Python 3.11 or newer.
- This mod's release package for your computer (the same one Ryujinx players use).
- Windows: Windows 10 or later, a graphics card with Vulkan 1.3.

## Windows

1. Get the Savage Wind source that loads code mods:

   ```bat
   git clone -b coop-mods https://github.com/Jaiden56YT/SavageWindRecomp.git
   ```

2. Install its build tools. In PowerShell, in the `SavageWindRecomp` folder:

   ```bat
   powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
   ```

   This installs MSYS2 (the compiler) and Python 3.12, and takes a while.

3. Put your game files in it: `SavageWindRecomp\extracted\exefs` and `SavageWindRecomp\extracted\romfs`.

4. Put the mod in. In a Command Prompt in this package's folder:

   ```bat
   setup.bat --recomp "C:\path\to\SavageWindRecomp"
   ```

   No dump files and no console keys are needed here: the files it needs are copied from `extracted\romfs`. It builds the puppet actors, puts the mod's code beside the game's, and writes `play-coop.cmd` and the settings file.

5. Open the settings file it names (`coop\romfs\botw-coop\config.ini`) and set `name`, `server` and `password`, as in [PLAYING.md](PLAYING.md).

6. Build the game with the mod in it. Open **MSYS2 UCRT64** from the Start menu and run:

   ```bash
   cd /c/path/to/SavageWindRecomp
   make gen
   make -j
   ```

   The first build takes 15 to 20 minutes.

7. Start `play-coop.cmd` in the `SavageWindRecomp` folder and load a save. A banner says "Co-op: connected".

## Mac (Apple silicon)

The same, with Homebrew's tools in place of MSYS2 (`brew install sdl3 ffmpeg molten-vk vulkan-loader vulkan-headers shaderc`, and a Python environment in `tools/venv` with `lz4 capstone unicorn`):

```bash
git clone -b coop-mods https://github.com/Jaiden56YT/SavageWindRecomp.git
# put your game files in SavageWindRecomp/extracted/exefs and extracted/romfs
./setup.sh --recomp ~/SavageWindRecomp
cd ~/SavageWindRecomp
make gen
make MAC_VULKAN=1 B=build-vk -j
./play-coop.sh
```

## After an update of the mod

The mod's code is part of the built game, so every new version of the mod needs a rebuild: run `setup` with `--recomp` again from the new package, then `make gen` and `make -j` (a few minutes; only what changed is built again). Everyone who plays together still needs the same version of the mod.

## What's different from Ryujinx

- **Saves.** Savage Wind keeps its own saves (the `save` folder). The mod's separate co-op copy lives inside them, as everywhere else. To bring a save over from Ryujinx or a Switch, see Savage Wind's `docs/save-converter.md`.
- **Far warps arrive.** The warp that often gets stuck in Ryujinx over several kilometres arrived every time in the recomp on a Mac. `warp_direct = true` in `config.ini` makes it one jump there.
- **Network.** Nothing to turn on: the mod uses the computer's own network.
- **The log** is in the window `play-coop` runs in. The mod's lines start with `[coop]`, the loader's with `[mods]`.

## If it doesn't work

| What happens | Likely cause | What to do |
|---|---|---|
| `setup` says the folder "isn't the Savage Wind source that loads code mods" | It's the official source or the official download. | Clone the `coop-mods` branch as in step 1. |
| `make gen` or `make` stops with an error | A build problem on your system. | Copy the last 30 lines and share them. |
| The game starts, but there's never a "Co-op:" banner | The mod isn't in the game code. | Look for `[mods] subsdk9 is a mod` in the log. If it isn't there, run `setup --recomp` and both `make` steps again. |
| The log names a `main+...` line to "add to hooks.txt", other than `main+c61540` | The mod hooks a game function the game code wasn't built for. (The same message for `main+c61540` and the `sdk+...` lines is normal.) | Your package and `extracted\exefs\hooks.txt` are from different versions: run `setup --recomp` again, then `make gen` and `make -j`. |
| "Co-op: can't reach ..." | As in Ryujinx: the address, the relay, or a firewall. | See [PLAYING.md](PLAYING.md). |
| A crash, or the game closes | Unknown: this is new ground on Windows. | Share the whole log window's text, and what you were doing. |

To play without the mod again: delete `subsdk9`, `subsdk9.mod` and `hooks.txt` from `extracted\exefs`, run `make gen` and `make -j`, and start Savage Wind the normal way.
