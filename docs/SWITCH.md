# BotW co-op on a real Switch

The mod runs on a modded Switch (Atmosphère) the same way it runs in Ryujinx: the same code, the same game files, in a different folder. A Switch player and a Ryujinx player can play together.

**First run on a real Switch: 2026-11-10/11.** On a new game it loaded, read its settings, connected to the relay over Wi-Fi, and showed a Ryujinx player as a second Link with the co-op banners; the Ryujinx player saw the Switch player too. Not yet tried on hardware: a large existing save (the save journal limit and the backup), trades, going down, enemy sync, and long sessions. See [What's different on a Switch](#whats-different-on-a-switch), and follow [If it doesn't work](#if-it-doesnt-work) so a cause can be found.

## What you need

- A Switch running **Atmosphère** (any recent version; Kefir and other packs are Atmosphère).
- **Your own** Breath of the Wild on it with the **1.9.0** update, the same version the mod was built for. The mod turns itself off on any other version.
- Wi-Fi, and a computer on the same network running the relay (or any relay the Switch can reach by IP address).
- An SD card reader, or FTP/MTP to the Switch, to copy files.

## Before you start: back up your save

On a Switch the mod keeps co-op in a separate copy of your saves inside the game's save data, just like in Ryujinx, and never writes to your normal slots. But it has only had a short run on real hardware. **Back up your BotW save first** with JKSV or Checkpoint. It takes a minute and makes any mistake undoable.

## Install

1. Build the Switch files from your own dump (once; they contain game files, so they can't be shared):

   ```bash
   tools/setup.sh "<base game .nsp or .xci>" "<1.9.0 update .nsp>" --switch dist/switch-sd
   ```

   (Windows: `setup.bat ... --switch dist\switch-sd`.) This writes `dist/switch-sd/atmosphere/contents/01007EF00011E000/` with:

   | Folder | What |
   |---|---|
   | `exefs/subsdk9` | The mod's code. |
   | `exefs/main.npdm` | The game's permissions with the extra ones the mod's hooks need. Required on a real console. |
   | `romfs/` | The puppet actors built from your game files, and `botw-coop/config.ini`. |

2. Edit `dist/switch-sd/atmosphere/contents/01007EF00011E000/romfs/botw-coop/config.ini`:

   | Setting | What to put |
   |---|---|
   | `name` | The name your friends see. |
   | `server` | The address of the computer running the relay. The relay prints it when it starts ("for players on the same Wi-Fi"). It must be numbers like `192.168.1.20`, not a name. |
   | `password` | The same word as your friends. |
   | `backups` | Leave at `1`: a Switch's save data only has room for one. |
   | `log_file` | Leave at `true` for the first tries: the mod writes what it does to `sd:/botw-coop/log.txt`. |

3. Copy the `atmosphere` folder onto the SD card, merging it with the one that's there. The result on the card is `sd:/atmosphere/contents/01007EF00011E000/exefs/subsdk9` and so on.

4. If you have other BotW mods in that same folder, move them out for the first try. A mod with its own `exefs` (another `subsdk9` or `main.npdm`) can't be combined with this one.

5. Start the relay on the computer, then start BotW on the Switch **normally** (not while holding R, which opens the homebrew menu). Load a save.

## What you should see

1. The game starts as usual. The very first start may sit on a black screen a few seconds longer: the mod is copying your saves into the co-op copy.
2. Your save list shows the same saves as before (they're the copy).
3. A few seconds after you're in the world, a banner at the top: **"Co-op: connected. Waiting for a partner"**, or **"Co-op: \<name\> is here"**.
4. The relay's window on the computer shows your name joining.

## If it doesn't work

| What happens | Likely cause | What to do |
|---|---|---|
| The game crashes when it starts (Atmosphère error screen) | The mod's code or `main.npdm` didn't load right, or another mod clashes. | Note the error code. Copy the newest file from `sd:/atmosphere/crash_reports/` and `sd:/botw-coop/log.txt` (if there is one) to the computer and share them. Remove other BotW mods and try again. |
| The game runs but there's never a "Co-op:" banner | The mod isn't loading (wrong folder name, or the game isn't 1.9.0), or it turned itself off. | Check the folder is exactly `atmosphere/contents/01007EF00011E000/`. Look in `sd:/botw-coop/log.txt`: no file at all means the mod's code never ran. |
| "Co-op: no network connection" | The Switch has no Wi-Fi connection. | Connect to Wi-Fi (airplane mode off). |
| "Co-op: can't reach the server at ..." | The address is wrong, the relay isn't running, or a firewall blocks it. | Check `server` against what the relay printed. On a Mac, allow the relay in the firewall prompt (System Settings → Network → Firewall). The Switch and the computer must be on the same network. |
| "Co-op is off: ... different versions" | Your friend's mod or the relay is another version. | Everyone uses the same release package. |
| The game starts a **new game** or shows no saves | The co-op copy went wrong. | Close the game **without saving**. Your normal saves are untouched: delete the mod's folder from the SD card, start the game, and they're there. Then share `log.txt`. |
| Stutter near some enemies | The mod looks through memory for an enemy's physics body, a few milliseconds per frame. | Note where and which enemy; the search can be tuned. |

To remove the mod: delete `sd:/atmosphere/contents/01007EF00011E000/` (or just its `exefs` folder to turn the code off). The game goes back to your normal saves. The co-op copy stays inside the save data as the folders `coop` and `coop_backups`, which the game ignores.

## Playing with a friend on Ryujinx (or another Switch)

- One relay, on a computer. Everyone connects to it.
- The Switch connects over your home network to the computer's local address (`192.168.x.x`).
- A friend in another home connects to the same computer over the internet: see "Playing over the internet" in [PLAYING.md](PLAYING.md).
- Everyone needs the same mod version and the same `password`.

## What's different on a Switch

These are the Switch-only things, and how each was handled:

| Thing | In Ryujinx | On a Switch | Status |
|---|---|---|---|
| Hook permissions | Not enforced. | The mod's hooks need system calls a game normally isn't allowed (`MapProcessMemory` and others). `exefs/main.npdm` grants them. | The file matches the game's own (title id, services, flags) plus those calls. **Runs on a Switch.** |
| Save journal | Unlimited. | BotW's save data is 62 MB with a 1.5 MB journal: no more than that can be written between two commits. | The first copy and the backups now commit after every megabyte. Works in Ryujinx, and on a Switch with a new game's small save; **a large save has not been tried on hardware.** |
| Save space | Unlimited. | A full 100% save is about 16 MB, so normal saves + co-op copy + 1 backup is 48 MB of 62. | `backups = 1` on a Switch. If a backup doesn't fit it's dropped, not left half-made. |
| Network | "Guest Internet Access". | The mod asks the system for the network and starts sockets itself (the same calls other Switch online mods use). | **Runs on a Switch** (Wi-Fi to a relay on the same network). |
| Speed | A Mac is several times faster. | Searching memory for an enemy's physics body took up to 170 ms on the Mac. | The search is now spread over frames, 3 ms at a time. Finding a body may take a few seconds on a Switch. |
| Log | Ryujinx's console. | Nothing visible. | `log_file = true` writes `sd:/botw-coop/log.txt`; connection problems also show as banners. |
| Two-game quirks | Two Ryujinx windows on one Mac can hang on loading. | Doesn't apply. | — |
