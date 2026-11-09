# Reverse-engineering notes: BotW 1.9.0 (Switch)

Everything the mod relies on in the 1.9.0 executable, how it was found, and the emulator quirks met along the way. The authoritative list of addresses is `client/source/program/game/offsets.hpp`; this page explains them.

## The build

- Update `01007EF00011E800`, version `v1114112`, display version **1.9.0**.
- Main executable build ID: `cd57b23fa4bbad65803d9788c01821ee`. The mod reads the GNU build-ID note at the end of `.rodata` and refuses to run on anything else.
- Built with **Nintendo SDK 20.5.22**, from a path ending `Product_CSPGO_Optimize`, so it's a profile-guided build. Code layout and inlining differ a lot from the 1.5.0 build the [zeldaret/botw](https://github.com/zeldaret/botw) decomp targets. The decomp's **names, logic and struct layouts** still carry over (every struct checked so far has the same offsets), but its **addresses** don't.
- Text segment: `0x0`–`0x15a1c64`. Rodata: `0x15a2000`–`0x1b79330`. Data: `0x1b7a000`–`0x1db21d8`. BSS: `0xe5e28` bytes after data.

## How things were found

The method that keeps working: find a **string** the decomp shows a function uses, find the code that loads it (`ADRP` + `ADD`, see `tools/xref.py str`), and read the surrounding code against the decomp. Then follow calls and global loads from there.

Singletons are reached through **GOT slots**: `adrp x8, page; ldr x8, [x8, #off]; ldr x0, [x8]`. The mod reads the slot at runtime (it holds the relocated address of `sInstance`) instead of hard-coding the instance address.

| What | Where | Found from |
|---|---|---|
| `PlayerInfo::sInstance` | `main+0x1e13ed0` (BSS) | `getPlayerPos` (uses the string `"getPlayerPos"`, at `0xcc7598`); all its callers load this global. |
| `PlayerInfo` fields | actor `+0x60`, pos `+0x88` | Getter bodies next to `getPlayerPos`. |
| `Actor::mMtx` | `+0x398` | Same offset as the decomp; confirmed by an actor-spawning caller that builds `@P` from it. |
| `ActorCreator::createActor` | `0x1535900` | `ActorTemplate::getActorClass` (`0x13d12dc`, logs `"[%s] classの定義がありません"`) has three callers inside `ActorCreator`; this one takes `(name, heap, params, sleep, block)` and returns `Actor*`. |
| `ActorCreator::sInstance` | GOT `0x1d66550` | Callers of `createActor`. Its heap argument is `*(*(GOT 0x1d66558) + 40)`. |
| `ActorCreator` actor list / lock | `+0x38` list, `+0x4c` node offset, `+0xa8` lock | `enableDistanceUnloadChecks` (`0x153619c`, uses `"ArmorExtra"`). |
| `sead::CriticalSection` lock / tryLock / unlock | `0xeaff08` / `0xeaff10` / `0xeaff18` | Thunks called from the function above. |
| `Actor` no-distance-unload flag | `+0x518` bit `0x80` | Set by `enableDistanceUnloadChecks`. |
| `InstParamPack` vtable | `0x1bc5540` | Stack-constructed packs in `createActor` callers. Layout: vtable, `mProc`, `u16 count`, `s16 pos`, `u8 data[0xc0]`. Entries are `const char* key, u8 type, value`. |
| `gdt::Manager` | GOT `0x1d66478` | `TimeMgr::init` (`0x1425664`, uses `"WM_Time"`, `"WM_NumberOfDays"`). |
| Flag handle | `index | (mgr[+0xc28] << 24)` | Inlined `getF32Handle` in `TimeMgr::init`. The lookup table is `**(mgr + 0xbd8)`, so it's dereferenced twice. |
| Flag name hash | `0xeb7a84` | Same. |
| Index lookups | bool `0x1143950`, s32 `0x1143958`, f32 `0x1143960` | Thunks into one sorted-hash search at `0x113226c`. |
| get/set flag value | getBool `0x6b6980`, setBool `0x64af38`, getS32 `0xb4042c`, setS32 `0x6b6b3c`, getF32 `0xb366a0`, setF32 `0xb36c80` | `TimeMgr::calc_` (`0x14260b4`). Getters are `(mgr, out*, handle)`; setters are `(mgr, value, handle)`. |

### Things that turned out to be wrong

- **"A player actor exists" doesn't mean "in the world".** The title screen renders the Great Plateau with a Link actor parked at exactly `(-1060, 250, 1830)`. The mod treats that exact position as the title screen.

- **`PlayerInfo::mPlayerPos` is `(0,0,0)` until the world has loaded.** The player actor already sits at the save position during the loading screen, but `mPlayerPos` only becomes non-zero once gameplay starts. The mod uses that as its "in the world" signal. It reads the position and facing values themselves from the player actor's matrix (`PlayerInfo+0x60` → `Actor+0x398`).
- **Spawning actors during the loading screen stalls it.** Seven synchronous `createActor` calls mid-load left BotW on the loading screen indefinitely. Wait for the signal above.

## Spawning and moving actors

- `createActor` is synchronous and can be called from game-logic threads. The mod calls it from a hook on `PlayerInfo::getPlayerPos`, which game logic calls every frame from several threads. An atomic flag makes sure only one ghost tick runs at a time, including re-entry while an actor is being created.
- Writing `Actor::mMtx` moves a spawned cutscene actor and it stays where it's put. The champion spirit (`Dm_Npc_Zora_HeroSoul_Kago`) walks itself to Link for its first few seconds (its "gift" AI), then holds. `PauseMenuPlayer` holds from the first frame.
- Spawned actors can be deleted by the game, for example on stage loads. Before touching its ghost each frame, the mod checks the actor is still in `ActorCreator`'s live list, under `tryLock`; if the lock is busy it skips that frame. A new player-actor pointer means a stage load, so every ghost is forgotten and respawned.

### What actually shows on screen (tested in the Shrine of Resurrection)

| Actor | Draws? | Follows `mMtx` writes? |
|---|---|---|
| `Npc_King_Vagrant001` (Old Man), `Npc_DungeonPriest001` (Sheikah monk) | **Yes** | **No.** They have a physics body (character controller), and the game copies its position back into `mMtx` most frames, so they stay at the spawn point. |
| Apple, sword (`Item_Fruit_A`, `Weapon_Sword_001`) | Yes (an "A Take" prompt appears) | Not tested (physics objects). |
| `Dm_Npc_*_HeroSoul_Kago` (champion spirits), `Dm_Npc_RevivalFairy`, `PlayerStole2`, `PauseMenuPlayer` | No | Yes, no physics. But their AI or item logic keeps them hidden until something triggers them. |

The fix that worked is a **custom actor pack**. `CoopGhost` is `Npc_DungeonPriest001` with `PhysicsUser`, `AttentionUser` and `ChemicalUser` set to `Dummy`. It draws like the monk, and the game never moves it: 0 of 909 frames. `tools/build_romfs_mod.py` builds it:
- copies the template pack and rewrites its ActorLink
- inserts an `ActorInfo` entry in CRC32 order, with the `Hashes` array kept parallel
- adds resource-size-table entries for the new pack and ActorLink

`createActor` loads the new pack from the RomFS on demand. `ActorInfo.product.byml` itself has no size-table entry, so making it bigger is fine.

The Link puppet will be built the same way: Link's model and animations, no physics.

## Game files

- 1.9.0's `ActorInfo.product.sbyml` lists 7,944 actors (the base game's lists 7,307). It's only in the update's patched RomFS. `tools/RomfsTool` reads it with the same LibHac Ryujinx uses.
- Only two actors use Link's model: `GameROMPlayer` (the real player, which can't be spawned twice) and `PauseMenuPlayer`.
- Many cutscene actors (`DemoNPC`), including the champion spirits, live inside `Pack/TitleBG.pack`. It's loaded at boot, so they can be spawned without waiting on resource loads.

## Ryujinx / Nextendo quirks

| Problem | What the mod does |
|---|---|
| SDK-20 `rtld` only accepts `0`, `0xEA000000` or `b +8` as a module's first word; anything else makes it read `+8` as a header offset and crash. | `crt0.s` starts with `b +8`, then the MOD0 offset, then a branch to the real entry. |
| `nn::ro::detail::g_LookupGlobalManualFunctionPointer` doesn't exist in this game's rtld. | Weak import, checked before use. |
| Receive timeouts on blocking sockets are ignored. | Never block: `poll` for 5 ms, then a non-blocking `recv`. |
| `recv` into a buffer much larger than 4 KB returns 0 bytes, which looks like the peer closing. | Reads are capped at 4 KB. |
| A `poll` timeout returns `-1` with `ETIMEDOUT` (110), not `0`. | Treated as "no data yet". |
| Mounting the SD card fails (`0x320002`); the game isn't allowed to. | Settings are read from the mod's own RomFS (`content:/botw-coop/config.ini`), falling back to the SD card. |
| Ryujinx reports `EAGAIN` as 11 (Linux numbering). | 11 and 35 (BSD) are both accepted. |
| **A second Nextendo instance hangs on BotW's loading screen while the first is running**, with or without mods. It stops right after the game's load-time play report and never creates Link. Every load started while the other instance was paused finished in about 20 s. Pausing after the hang doesn't recover it; the load has to start while the other instance is paused. Turning off Discord integration didn't help on its own. | Pause the other instance while a save loads. Only affects two games on one machine. |
| Two instances can share a machine with `--root-data-dir`. A copied data folder needs Nextendo's `nextendo_patchnote_version` too, or launch stops at a "what's new" popup. | Documented in README. |
