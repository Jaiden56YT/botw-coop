# botw-coop

Seamless co-op for *The Legend of Zelda: Breath of the Wild* on Ryujinx (Switch v1.9.0). See [DESIGN.md](DESIGN.md) for what it will play like and how it works, and [docs/reverse-engineering.md](docs/reverse-engineering.md) for what's known about the 1.9.0 executable.

## Status

**Playable co-op core: you see each other, share progress, the world and fights.** Another player appears in your world as a Link puppet that goes where they go and moves the way they move; their progress, the host's time and weather, and damage to enemies carry over between your games.

- The mod loads into BotW 1.9.0 and checks the game version.
- It connects to the relay and reconnects on its own if the connection drops.
- Each player's Link position and facing are shared about 30 times a second.
- Each other player is a `CoopPlayer` actor, added to the game by a RomFS mod: Link's model with no physics, so it goes exactly where the mod puts it (the game moved it in 0 of 909 frames). It survives stage loads.
- **The puppet shows what the other player is actually doing.**
  - Each game reads its own Link's animation system, sorts it into a movement state, and sends that state.
  - States: standing, moving (walk, run or sprint by speed), climbing, clinging, swimming, swimming in place, fast swimming, gliding, jumping, falling, riding a horse, shield surfing.
  - Climbing and swimming were seen on screen.
  - **Fighting and items too:** sword combos (each hit of the combo and the finisher), spin attack charge and release, drawing and shooting the bow, winding up and throwing a weapon, and raising the shield. These come from Link's own clips and play over the movement state; checked on screen with a fake partner cycling through them.
- **The puppet wears the partner's armor.** Worn armor pieces are separate actors on Link; each game sends their names and the others show the same pieces on the puppet. The build makes one gear actor per armor model (116 models cover all 413 armor pieces), which plays the puppet's animations so it moves with the body. Checked: a fake partner in the Hylian hood, tunic and trousers showed up dressed in them, and two real games each dressed the other's puppet.
- **And carries their weapons.** Each game works out which of Link's bones each carried weapon, shield, bow, sheath and quiver sits on (the hands, the back holsters `Pod_A`/`Pod_B`/`Pod_C`, spine or waist) and its offset from that bone, and sends that when it changes. The partner's game binds a weapon-model actor to the same bone of the puppet with the game's own bind code, so it follows the puppet's animation: on the back when sheathed, in the hand when drawn. Checked with echo mode (a puppet of yourself) carrying sword, shield, bow and quiver on its back just like the real Link, and between two real games (each puppet got all five items).
- **Areas:** each player sends which area they're in (the overworld, or a particular shrine or Divine Beast). A partner in another area is hidden instead of showing at meaningless coordinates. Checked with a fake partner walking into a shrine and out again.
- **Riding:** while a partner rides, their puppet sits in the saddle of a horse (a physics-free `CoopHorse` built from a game horse's model and animations). The horse stands, walks, trots or gallops with their speed, and the rider plays Link's matching riding animation. The saddle position comes from the horse model's `Saddle_Root` bone (2.1 m up). Checked with a fake rider: the puppet sits astride the horse, facing the same way. It's always the same brown horse, not their own.
- **Trading weapons:** drop or throw a weapon so it lands within 3 m of your partner, and it leaves your world and appears at their feet in theirs. That works for a weapon you're carrying and for one you drop from the inventory. Only your own weapons count (a weapon you carried a moment ago, or one that appeared by you as the menu closed), so enemy drops aren't sent. Checked: a fake partner's gift landed at player 2's feet, and a sword player 2 dropped from the inventory next to the fake partner was handed over and removed. Note the Master Sword can't be thrown (it fires a beam instead).
- **Trading food and materials:** BotW's inventory can't drop materials, only hold them. Choose **Hold** on an item, close the menu, and put it down (A) or throw it next to your partner. Once it's out of your hands within 3 m of them, it moves to their world and lands at their feet. A stack you hold goes over item by item. Cooked dishes can't be held, so they can't be traded. Checked through the real menu: player 2 held an Octorok Eyeball, put it down next to a fake partner, and it went to them; a fake partner's apple landed at player 2's feet.
- **The host's Blood Moon is shared:** guests follow the host's Blood Moon timer and "Blood Moon tonight" flag, so with the shared clock it rises the same midnight for everyone. Checked: a guest with the host's flag played the Blood Moon cutscene at midnight.
- **The host's time of day and weather are shared.** Weather is only followed inside the same climate (no snow in the desert because the host is on a mountain). Checked: a guest under clear sky turned to the host's thunderstorm.
- **Fights count for both.** Enemies placed in the world have the same id in every player's game. When one is hurt in your game, its new life is sent to the others, and their copy drops to the same life; at zero it dies there too, with its normal death and drops. Checked: a partner's kill of a Stone Talus removed it (and its five parts) from player 2's world. Enemies that aren't placed (random spawns) aren't shared.
- **Co-op revive:** when a hit would kill you, the game's own revives (a fairy, Mipha's Grace) still come first. If you have none and a partner is with you (connected, same area), you get back up with 3 hearts instead of a game over, at most once every 2 minutes, and your partner's game is told. Checked with a real hit: at a quarter heart with fairies skipped, a Bokoblin's hit brought player 2 down, they got back up with 3 hearts, and the fake partner got the message.
- Each player's hearts are sent with their position (for a future health display).
- **Shared progress:**
  - What's shared: shrines found, activated and cleared (`Enter_`/`Open_`/`Clear_Dungeon*`), towers (`MapTower_*`), Divine Beasts (`Clear_Remains*`), and all 900 Koroks found (`MainField_Npc_HiddenKorok*`).
  - When one of these turns true in your game, everyone else's game gets it too.
  - Players who join late get everything that's already done, and their own progress goes to everyone already there: a joiner with 15 towers the others lacked passed all 15 on (between two real games).
  - Flags are only ever set, never cleared.
  - **Rewards are shared for live progress:** when a partner clears a shrine you get a Spirit Orb; when they find a Korok, a Korok seed; when they clear a Divine Beast, that Champion's ability. Mipha's Grace was seen appearing in the HUD. Catch-up for late joiners marks things done but gives no rewards.
  - Checked with a 100% save: a late joiner is sent all 1,327 done flags, and a Spirit Orb from a partner's shrine shows up in Key Items.
  - **A partner's tower reveals your map.** Checked with a copy of the 100% save with every tower turned off: the map stayed dark with no partner, and was fully revealed after a partner shared the 15 towers.
- **Partners on your map:** each other player is a stamp on your map at their position, updated every second and removed when they leave. A partner inside a shrine stays marked at the shrine they went into. It's a real map stamp (stamp icon 34), made through the map's own code, so it's saved like other stamps if you save while a partner is around. The mod removes leftover partner stamps the next time the save is played. Your own stamps are never touched.

Up to four players can play together (the relay and the mod both handle it). Checked with three: player 2 with two fake partners got a puppet and a map marker for each, one dressed, one on a horse.

Tested with a scripted fake partner (`tools/fakehost`) and with **two real games** on one Mac: both connected through the relay, each showed the other's puppet and map marker, the guest followed the host's time, a late joiner got 1,327 done flags, and when one game dropped off it reconnected by itself while the other took over as host.

Not yet:
- warp to partner: writing Link's position (his actor, physics and Havok copies) doesn't move him, no stage-change call with a free position has been found, and fast travel runs through the map screen's UI; see docs/reverse-engineering.md
- enemies moving the same way in both games (only their life is shared): that needs taking over enemy AI
- name tags and a health display
- cutscene watch mode, fast-travel voting, trading cooked dishes
- separate co-op saves (for now, use a separate Ryujinx user profile for co-op, see below)

## Layout

| Path | What |
|---|---|
| `server/` | Relay server (Go). Rooms by password; forwards player state and game messages; hands the host role on if the host leaves. |
| `client/` | In-game mod (C++, built on [exlaunch](https://github.com/shadowninja108/exlaunch)). Builds to `subsdk9`. |
| `client/source/program/main.cpp` | Startup, version check, hook installation. |
| `client/source/program/net/` | Network thread and wire protocol. |
| `client/source/program/game/` | Game-side code: `offsets.hpp` (every 1.9.0 address and how it was found), `player` (local Link, area, hearts), `ghost` (remote players' puppets), `anim` (movement and action states), `gear` (worn armor), `map` (partner markers), `progress` (shared flags and rewards), `combat` (shared enemy life), `revive` (co-op revive), `weapons` and `items` (weapons on the puppet, trading), `world` (shared time and weather), `gdt` (game data flags), `autostart` (test helper). |
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

The puppet actors and the shared-flag list are built from your own game files. Nintendo's files are never part of this repo. Build it once after extracting the inputs:

```bash
B="<base game .nsp>"; U="<1.9.0 update .nsp>"
T="dotnet run --project tools/RomfsTool --"
$T "$B" "$U" extract Actor/ActorInfo.product.sbyml .gamefiles/ActorInfo-1.9.0.sbyml
$T "$B" "$U" extract System/Resource/ResourceSizeTable.product.srsizetable .gamefiles/ResourceSizeTable.product.srsizetable
$T "$B" "$U" extract Actor/Pack/Npc_DungeonPriest001.sbactorpack .gamefiles/packs/Npc_DungeonPriest001.sbactorpack
$T "$B" "$U" extract Actor/Pack/PauseMenuPlayer.sbactorpack .gamefiles/packs/PauseMenuPlayer.sbactorpack
$T "$B" "$U" extract Pack/Bootup.pack .gamefiles/Bootup.pack
$T "$B" "$U" extract Pack/TitleBG.pack .gamefiles/TitleBG.pack
$T "$B" "$U" extract-prefix /Actor/Pack/Armor_ .gamefiles/armor   # every armor actor, for the gear actors
$T "$B" "$U" extract-prefix /Actor/Pack/Weapon_ .gamefiles/weapons # every weapon actor, for the weapon actors
.venv/bin/python -I tools/build_romfs_mod.py .gamefiles client/romfs
```

`deploy-ryujinx.sh` copies `client/romfs` into the mod folder. It contains:
- the new actor packs
- an `ActorInfo` with them added
- a matching resource size table
- `botw-coop/sync-flags.txt`, the 1,330 progress flags that are shared
- 116 `CoopGear_<model>` actors and `botw-coop/gear-map.txt` (armor actor name → model), for showing partners' armor
- 265 `CoopWeapon_<model>` actors and `botw-coop/weapon-map.txt`, for showing partners' weapons

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
| `ghost_actor` | `CoopPlayer` | Actor that stands in for other players. `CoopPlayer` (Link, animated) and `CoopGhost` (a Sheikah monk) are added by the mod's RomFS (see below). |
| `echo` | `false` | Debug: show a ghost of yourself, 1 s behind, circling you, wearing your armor and carrying your weapons. Lets you check puppets with one game. |
| `autostart` | `false` | Test only: taps A at the title and menus until Link is in the world. |
| `test_spawn` | empty | Debug: comma-separated actors spawned once in a ring 3 m around Link, to check which actors draw. |
| `test_flag` | empty | Debug: sets this progress flag once in the world, the way the game would, to test sharing. |
| `test_move` | `false` | Debug: walks your Link forward and jumps, logging his animation sequences. |
| `test_menu` | `false` | Debug: opens the inventory 2 s after a partner's reward, and the map 15 s after a partner's marker appears, so they can be checked without a controller. |
| `test_bind` | empty | Debug: `Weapon:Bone` (e.g. `Weapon_Sword_070:Head`) spawns that weapon's actor and binds it to that bone of your Link. |
| `test_keys` | empty | Debug: buttons pressed once in the world, 1.2 s apart, e.g. `Plus,Right,A:300` (`Button:ms` holds it). `SL`/`SR`/`SU`/`SD` push the left stick; any other word just waits (`W:3000`). |
| `test_drop` | empty | Debug: an item (e.g. `Item_Fruit_A`) that appears at your feet 14 s after you're in the world, as if put down from the menu. |
| `test_revive` | `false` | Debug: once a partner is with you, sets your life to 0 and runs the game's revive check (skipping fairies), to test co-op revive. |
| `test_life` | `0` | Debug: once a partner is with you, drops your life to this many quarter hearts and skips fairies for the next lethal hit, to test co-op revive against a real enemy (pair with `test_spawn = Enemy_Bokoblin_Junior`). |
| `test_spawn` entries | | `Actor:ModelUser` spawns the actor with that model user (`@MU`); only some game classes honor it. |

## Play

1. One person runs the relay: `.tools/relay -listen :7777`. Other machines need its address in `server =`; use Tailscale or ZeroTier if you aren't on the same network.
2. Everyone launches BotW in Ryujinx and loads a save. The first player to connect is the host; their time of day and weather are the ones everyone shares.
   - Shared progress is written into the save you're playing. To keep your normal save as it is, make a second user profile in Ryujinx (Options → Manage User Profiles) and play co-op on that one: each profile has its own saves.
3. The relay prints joins, leaves and a position line every few seconds. The mod's own messages (starting `[coop]`) appear in Ryujinx's log.

### Two games on one Mac

Ryujinx can run a second copy with its own data folder:

```bash
/Applications/Ryujinx.app/Contents/MacOS/Ryujinx --root-data-dir ~/botw-coop/.ryujinx-p2 "<path to BotW .nsp>"
```

Give that folder a copy of your `system/` keys, `bis/` (firmware and saves), `profiles/`, `Config.json`, and Nextendo's `nextendo_setup_done` and `nextendo_patchnote_version`. In its `Config.json`:
- switch its input to the keyboard so it doesn't share your controller
- set `enable_discord_integration` to `false`

**Known problem:** a second Ryujinx-Nextendo instance often hangs forever on BotW's loading screen while the first one is running, with or without this mod. Pause the first game (Actions → Pause Emulation) while the second loads a save, then resume it. `tools/run-player2.sh` does all of this for you: it installs the build into `.ryujinx-p2`, copies your save there, and launches player 2. It suspends the first game until player 2 is in the world, then resumes it. Two players on two different machines don't have this problem.

Use `RYU_PATH=~/botw-coop/.ryujinx-p2 ./tools/deploy-ryujinx.sh` to install the mod there, and give it a different `name`.

### Testing with one game

`tools/fakehost` joins a room as the host. It sends a fixed time of day and a scripted player walking a circle, so a single real game can be tested as a guest:

```bash
cd tools/fakehost && go build -o ../../.tools/fakehost . && cd ../..
.tools/fakehost -password test -center=-1060,250,1830 -time 180
```

Set the game's `password = test`, and turn on `autostart` if nobody is at the controller.

`tools/run-player2.sh` works for any test copy: `P_DIR=.ryujinx-p1 tools/run-player2.sh` starts a second one, so two test copies can play together without your own setup. Test copies are muted (`P2_SOUND=1` to hear one), and while one loads, every other running Ryujinx is paused.

The fake host has options for each feature: `-shrine-at N` (walks into a shrine after N s), `-enemy HASH:LIFE` (reports a hit on a placed enemy), `-weather N -climate C` (hosts that weather), `-flags A,B` (shares progress flags), `-gear HEAD,UPPER,LOWER` (wears that armor), `-tour`, `-anims` and `-actions` (movement and action states), `-still` (stands still 6 m east of `-center`), `-time DEG` (start time; the clock then runs at game speed), `-bloody` (a Blood Moon tonight) and `-gift NAME` (gives every player that weapon or item after 15 s) and `-ride` (reports riding a horse). It logs what the game sends back (flags, enemy hits, revives).

To test with a different save than yours (for example a late-game save that has the map and inventory), give player 2 its own save folder. Use one with slots `0`–`7`, `option.sav`, and so on, as the game writes them. Your own save is never touched:

```bash
P2_SAVE=<save folder> tools/run-player2.sh
```

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
