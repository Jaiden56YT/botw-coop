# botw-coop

Seamless co-op for *The Legend of Zelda: Breath of the Wild* on Ryujinx (Switch v1.9.0). See [DESIGN.md](DESIGN.md) for what it will play like and how it works, and [docs/reverse-engineering.md](docs/reverse-engineering.md) for what's known about the 1.9.0 executable.

## Status

**M1 (Ghosts): working on screen.** Another player shows up in your world as a ghost (a Sheikah monk for now) that walks where they walk.

- The mod loads into BotW 1.9.0 and checks the game version.
- It connects to the relay and reconnects on its own if the connection drops.
- Each player's Link position and facing are shared about 30 times a second.
- Each other player is a `CoopGhost` actor: the Sheikah monk with its physics removed, added to the game by a RomFS mod. Without physics it goes exactly where the mod puts it (checked: the game moved it in 0 of 909 frames). It survives stage loads.
- The host's time of day is shared.

Tested in a real game against a scripted fake host: the ghost was seen on screen circling Link.

Next is M2 (a real Link puppet with animations and gear). See DESIGN.md.

## Layout

| Path | What |
|---|---|
| `server/` | Relay server (Go). Rooms by password; forwards player state and game messages; hands the host role on if the host leaves. |
| `client/` | In-game mod (C++, built on [exlaunch](https://github.com/shadowninja108/exlaunch)). Builds to `subsdk9`. |
| `client/source/program/main.cpp` | Startup, version check, hook installation. |
| `client/source/program/net/` | Network thread and wire protocol. |
| `client/source/program/game/` | Game-side code: `offsets.hpp` (every 1.9.0 address and how it was found), `player` (local Link), `ghost` (remote players), `world` (shared time), `gdt` (game data flags), `autostart` (test helper). |
| `client/source/program/config.*` | Reads `config.ini`. |
| `tools/` | Deploy script, fake host for testing, and reverse-engineering helpers. |

## Requirements

- BotW v1.9.0, dumped from your own Switch, installed in Ryujinx.
- Ryujinx with **Guest Internet Access** turned on (Options → Settings → Network). The mod was developed on Ryujinx-Nextendo 1.10.4.
- devkitPro with devkitA64 (`/opt/devkitpro`) and Go 1.23+ to build. Python 3 and .NET 10 for the reverse-engineering tools.

## Build and install

```bash
cd server && go build -o ../.tools/relay . && cd ..
DEVKITPRO=/opt/devkitpro make -C client
./tools/deploy-ryujinx.sh
```

The `CoopGhost` actor is built from your own game files. Nintendo's files are never part of this repo. Build it once after extracting the inputs:

```bash
B="<base game .nsp>"; U="<1.9.0 update .nsp>"
T="dotnet run --project tools/RomfsTool --"
$T "$B" "$U" extract Actor/ActorInfo.product.sbyml .gamefiles/ActorInfo-1.9.0.sbyml
$T "$B" "$U" extract System/Resource/ResourceSizeTable.product.srsizetable .gamefiles/ResourceSizeTable.product.srsizetable
$T "$B" "$U" extract Actor/Pack/Npc_DungeonPriest001.sbactorpack .gamefiles/packs/Npc_DungeonPriest001.sbactorpack
.venv/bin/python -I tools/build_romfs_mod.py .gamefiles client/romfs
```

`deploy-ryujinx.sh` copies `client/romfs` into the mod folder. It contains the new actor pack, an `ActorInfo` with it added, and a matching resource size table.

The deploy script copies the mod into Ryujinx's mods folder. On first run it also writes a default `config.ini` to:

```
~/Library/Application Support/Ryujinx/mods/contents/01007ef00011e000/botw-coop/romfs/botw-coop/config.ini
```

The file lives in the mod's RomFS folder because Ryujinx layers that into the game's own files. That way the game can read it without SD card permissions.

### config.ini

| Key | Default | Meaning |
|---|---|---|
| `name` | your Switch profile's name | Name other players see. Must be unique in the session. |
| `server` | `127.0.0.1` | Relay server's IPv4 address. |
| `port` | `7777` | Relay port. |
| `password` | `hyrule` | Session password. Everyone who uses the same password plays together. |
| `ghost_actor` | `CoopGhost` | Actor that stands in for other players until the M2 Link puppet lands. `CoopGhost` is added by the mod's RomFS (see below); most stock actors either stay hidden or get pulled back by their physics. |
| `echo` | `false` | Debug: show a ghost of yourself, 1 s behind, circling you. Lets you check ghosts with one game. |
| `autostart` | `false` | Test only: taps A at the title and menus until Link is in the world. |
| `test_spawn` | empty | Debug: comma-separated actors spawned once in a ring 3 m around Link, to check which actors draw. |

## Play

1. One person runs the relay: `.tools/relay -listen :7777`. Other machines need its address in `server =`; use Tailscale or ZeroTier if you aren't on the same network.
2. Everyone launches BotW in Ryujinx and loads a save. The first player to connect is the host; their time of day is the one everyone shares.
3. The relay prints joins, leaves and a position line every few seconds. The mod's own messages (starting `[coop]`) appear in Ryujinx's log.

### Two games on one Mac

Ryujinx can run a second copy with its own data folder:

```bash
/Applications/Ryujinx.app/Contents/MacOS/Ryujinx --root-data-dir ~/botw-coop/.ryujinx-p2 "<path to BotW .nsp>"
```

Give that folder a copy of your `system/` keys, `bis/` (firmware and saves), `profiles/`, `Config.json`, and Nextendo's `nextendo_setup_done` and `nextendo_patchnote_version`. In its `Config.json`:
- switch its input to the keyboard so it doesn't share your controller
- set `enable_discord_integration` to `false`

**Known problem:** a second Ryujinx-Nextendo instance often hangs forever on BotW's loading screen while the first one is running, with or without this mod. Pause the first game (Actions → Pause Emulation) while the second loads a save, then resume it. Two players on two different machines don't have this problem.

Use `RYU_PATH=~/botw-coop/.ryujinx-p2 ./tools/deploy-ryujinx.sh` to install the mod there, and give it a different `name`.

### Testing with one game

`tools/fakehost` joins a room as the host. It sends a fixed time of day and a scripted player walking a circle, so a single real game can be tested as a guest:

```bash
cd tools/fakehost && go build -o ../../.tools/fakehost . && cd ../..
.tools/fakehost -password test -center=-1060,250,1830 -time 180
```

Set the game's `password = test`, and turn on `autostart` if nobody is at the controller.

## Server tests

```bash
cd server && go test -race ./...
```

## Reverse-engineering helpers

```bash
python3 -m venv .venv && .venv/bin/pip install nsz lz4 oead
.venv/bin/python -I tools/extract_exefs.py <update.nsp> .gamefiles/exefs-1.9.0   # executable out of the update
.venv/bin/python -I tools/nso.py .gamefiles/exefs-1.9.0/main .gamefiles/main-1.9.0.bin
.venv/bin/python -I tools/xref.py .gamefiles/main-1.9.0.bin str getPlayerPos     # code that uses a string
.venv/bin/python -I tools/xref.py .gamefiles/main-1.9.0.bin calls cc7598         # callers of a function
dotnet run --project tools/RomfsTool -- <base.nsp> <update.nsp> extract Actor/ActorInfo.product.sbyml out.sbyml
.venv/bin/python -I tools/romfs.py - byml out.sbyml out.json                     # decode Yaz0 + BYML
```

These need your console's keys in `~/.switch/prod.keys`. Game files stay in `.gamefiles/`, which is git-ignored.
