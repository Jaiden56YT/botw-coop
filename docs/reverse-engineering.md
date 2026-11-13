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

### Game-data flags: permissions and the write path

`Manager+0xbd0` is a `TriggerParamRef`:

| Offset | Field |
|---|---|
| `+8` | `TriggerParam**` primary (writes go here) |
| `+16` | `TriggerParam**` secondary |
| `+24` | `mCheckPermissions` |
| `+25` | `mPropagateParam1Changes` |
| `+26` | `mChangeOnlyOnce` (setters refuse while set) |

- The manager's setters (`setBool(handle)` at `main+0x64af38`, then `main+0x64b134`) call **`TriggerParam::setBool(param, value, idx, checkPerms, bypassOneTrigger)` at `main+0x1138814`**. Every bool write ends there, including name-based writes from events. The mod hooks it to notice progress.
- In gameplay `checkPerms` is 1. Progress flags (shrines, towers, Koroks) aren't writable from program code, only from events, so the plain setter returns 0 and does nothing.
- The mod writes them by calling `TriggerParam::setBool` with `checkPerms = false` on both tables. Read-back confirms it sticks.
- **Flag writes during loading or on the title screen don't stick.** Received flags wait until Link is in the world.
- **The flag tables reset (`Manager+0xc28` changes) before the save's values arrive.** Reading "what's done" at that moment sees nothing. The mod re-reads the shared flags each time Link enters the world, and shares only writes made while he's in it. Flags that turned true during a loading screen in the same save are shared on the next world entry. Found with a 100% save: before the fix it sent a late joiner 0 of its 1,327 done flags, and a game rewriting an already-true flag would have handed the partner a free reward.
- The shared list comes from `GameData/gamedata.ssarc` in `Pack/Bootup.pack` (45,027 flags, 42,027 of them bool):
  - shrines: `Clear_Dungeon000`–`136`, plus `Open_`/`Enter_` (found/activated)
  - towers: `MapTower_01`–`15`
  - Divine Beasts: `Clear_Remains{Electric,Fire,Water,Wind}`
  - Koroks: `MainField_Npc_HiddenKorokGround_<id>` (567) and `…Fly_<id>` (333)
  - counters: `KorokNutsNum` and `HiddenKorok_Number` (s32)

### Giving items

- **`uking::ui::PauseMenuDataMgr::itemGet(this, const SafeString& name, int count, const WeaponModifierInfo*)`** is at `main+0xd921bc`. It's what runs when Link picks something up: it adds the item to the pouch and saves the pouch to game data.
- Found from the `"PouchGet"` play report it sends. Its body has the decomp's "Default"/"Extra" name checks, `getType` (`main+0xd91414`) and `addToPouch` (`main+0xd92488`).
- The manager's GOT slot is `main+0x1d67af0`. The game's own give-one-item helper (`main+0xbe0f10`) calls it with count 1 and no modifier, and the mod does the same.
- Rewards: Spirit Orb `Obj_DungeonClearSeal`, Korok seed `Obj_KorokNuts`, Champion abilities `Obj_HeroSoul_{Zora,Goron,Rito,Gerudo}`. Giving `Obj_HeroSoul_Zora` made Mipha's Grace appear in the HUD at once.
- Before Link has the Sheikah Slate (the Shrine of Resurrection save), the pause menu can't be opened, so the pouch can't be inspected there.

### Map stamps (partner markers)

**The map keeps its own stamp objects. Game data is only a copy.** Writing `MapApp_MapIconNo`/`MapApp_MapIconPos` changes nothing on screen (tried: the arrays were written and read back, and the map didn't change).

- **Map manager:** the instance pointer is the global at `main+0x1e17ef8`. The map screen holds the same object at `screen+0x420`.
  - `+0x610`/`+0x618`: `PtrArray` of stamp objects, a pool of 100.
  - `+0x6a0`: linked list of saved stamps (records point at stamp objects).
  - `+0x2b0`: list of stamps whose visibility changed.
- **Stamp object:** vtable `0x1cdd340`.
  - Fields: icon (s32) at `+0x1c`, position (Vec3f) at `+0x28`, flags (u32) at `+0x3c`.
  - Flag bits: `0x10` active, `0x20` active changed, `0x8` placed, `0x200` visible, `0x4000` changed.
  - Every stamp in a loaded save has flags `0x7638`.
- **Functions:**
  - `0xd868d0`: loader. Empties the pool and adds every used game-data slot through `0xd8b6b8`. It resets far more than stamps, so the mod doesn't call it.
  - `0xd8a034`: writer. Copies the saved list back into the game-data arrays.
  - `0xd8b6b8`: maps the icon to a category through the table at `0x16d9f78` (stride 8). Category 21 (icons 27–36) goes to **`0xd8aaa0(mgr, icon, const Vec3f* pos, bool)`**, which takes a free pool stamp, sets its icon and position, and activates it.
  - Stamp vtable `+0xb8` = `0xd7b528`: `setPos(const Vec3f*)`.
  - Stamp vtable `+0xc8` = `0xd81d04`: `setActive(bool)`. Calls `+0xd8` (`0xd7b9bc`, sets the 0x10/0x20 bits), then registers or unregisters the stamp in the saved list and runs the writer.
  - `0xd7b8e4(stamp, bool)`: `setVisible`.
- **Icons:** the stamp menu (`0xdbab6c` → `0xd8ac88` → `0xd8a960`) only places icons 27–35 and 37–41. **Icon 36 is never drawn,** even though it shares a category with 27–35. The mod uses 34. Icon 42 is a separate category (`0x17`).
- **The game-data handles** are globals filled by `initCommonFlags` (`0xd19188`): `MapApp_MapIconNo` at `0x1da6d60`, `MapApp_MapIconPos` at `0x1da6d64`. Accessors: `setFlag_MapApp_MapIconNo(value, idx, debug)` at `0xd37790`, `setFlag_MapApp_MapIconPos(const Vec3f&, idx, debug)` at `0xd37810`, getters at `0xd37770`/`0xd377d0`.
- **TriggerParam layout matches the 1.5.0 decomp.**
  - The `getXIdx` stubs at `0x1143950`+ add `0x8`, `0x18` … `0x78` to the param, then branch to one search.
  - The array search at `0x1143990` reads the bool-array table at `+0x98`, so S32 arrays are at `+0xa8` and Vec3f arrays at `+0x108`.
  - `FlagS32` has its value at `+0x18` and hash at `+0x1c`; `FlagVector3f` has its value at `+0x30` and hash at `+0x3c`.
  - Array index lookups: `getS32ArrayIdx` `0x1143a20`, `getVec3fArrayIdx` `0x1143cf0`.
- **The mod's approach (`game/map.cpp`):**
  - Add with `0xd8aaa0`, then toggle `setVisible`. Move with `setPos` once a second. Remove with `setActive(false)`, which updates game data too.
  - Its stamps carry `0xA` in the low 4 mantissa bits of each coordinate. When a save is played, any of those still in it (the game saved while a partner was shown) are removed.
  - Seen on screen: a partner stamp 400 m north of Link appeared on the map.
  - Not checked: whether the open map follows a moving partner, or only updates when reopened.

### Areas (which stage a player is in)

- `ksys::StageInfo` statics are `FixedSafeString<32>`s (text pointer at `+8`): current map type at `main+0x1dae3a0` and current map name at `main+0x1dae3d8`.
  - Found from their getters, `0x1208304` and `0x12082f8`. The map manager compares the first one with "MainField".
  - In the overworld they read `MainField` / a grid section like `C-3`; in a shrine, `CDungeon` / `Dungeon000`.
- The mod's area id is CRC32("MainField") for the whole overworld, and CRC32(type + name) anywhere else.

### Actors: names, ids, life

The Actor layout matches the 1.5.0 decomp (`mASList` at `+0x560` agrees):
- `+0x10`: the name's text pointer (`BaseProc::mName`, a `FixedSafeString<64>` at `+0x8`).
- `+0x640`: `mHashId`, the map placement id. **It's the same in every player's game**, so it identifies "the same enemy" across worlds. Spawned actors and sub-parts have 0.
- Virtuals: `getMaxLife` (slot 30, vtable `+0xf0`), `killWithDropsAndEffects(int)` (slot 54, `+0x1b0`), `s32* getLife()` (slot 87, `+0x2b8`). The decomp names unknown virtuals `mNN` by slot, which is how these were counted.
  - Checked on Link: `getLife` reads 120/120 on a 30-heart save. For Link and enemies the life field is at `+0x848`.
  - `killWithDropsAndEffects(0)` on a Stone Talus removed it and its parts, with its normal death.
- **Setting Link's life to 0 doesn't kill him.** The hearts go empty and he keeps standing. Death is decided in the player's damage handler (`0xca8f44`). At `0xca93f8` it checks whether `*getLife() <= 0` and, if so, calls **`tryRevive(player)` at `0xcead24`**; false goes to the game-over path (`0xca9b84`).
  - `tryRevive` first checks a condition (`0xd358f0`), then player vtable `+0xa78` (Mipha's Grace; the path that plays "Kago_Heal"), then `+0xb50` (a fairy: removes `Animal_Insect_F` if the pouch has one, and plays "FairyResurrection").
  - Found from the `Animal_Insect_F` string, which `0xcead64` (inside `tryRevive`) references.
  - The mod hooks `tryRevive`: if the game's own revives fail and a partner is with you, it sets life to 3 hearts and returns true.

### Player animation sequences for actions

Logged with `test_move` (which presses the buttons) and the `ASList_StartAnimation` hook, per slot (0 body, 1 upper body, 2 weapon, 3 face):
- Sword: `WeaponEquipOn` (slot 2) draws the weapon. `CutS1`, `CutS2`, `CutS3` and `CutSF` (finisher) are the swings on slots 0 and 1. `CutChargeWait` is the spin charge. `WaitAttention` / `TurnAttention` is the combat stance.
- Bow: `BowCharge` then `BowShoot`, on slot 1 only.
- Throw: `WeaponThrowCharge`.
- Shield: only the face slot shows it (`FaceShieldGuard`).
- Matching clips in `Model/Player_Animation.sbfres`: `Sword_Attack_S1`/`S2`/`S3`/`SF`, `Sword_Attack_Charge`, `Sword_Charge_Wait`, `Bow_Attack_Charge`, `Bow_Attack_Shoot`, `Sword_Throw_Charge_Wait`, `Sword_Throw_Throw`, `Shield_Guard_Wait`. They work on the puppet as one-clip sequences, like the movement ones.

### Worn armor

- **Worn armor and weapons are separate actors sitting on Link** (distance 0): `Armor_<id>_Head`/`_Upper`/`_Lower`, `Weapon_Sword_…`, `Weapon_Shield_…`, `Weapon_Bow_…`, `Weapon_Quiver_…`, `Weapon_Sheath_…`. There are also two `Armor_Default_Extra_*` actors.
- An armor actor's pack holds one `Actor/ModelList/<model>.bmodellist`; its ActorInfo `bfres`/`mainModel` name the model archive. 413 armor pieces use 116 models (upgrades share one).
- **An actor with an armor's model list and the puppet's ASList animates the armor with Link's clips.** The ASList's `AddRes` is `Player_Animation`, and armor skeletons use Link's bone names. That's what the `CoopGear_<model>` actors are.
- `@MU` (model user override, InstParamPack string type 5) is written by the game for helper actors, e.g. shield surfing's `WakeBoardPlayer` and arrows. **Our NPC-profile actors ignore it**, even with `ModelUser = Dummy`, so each model needs its own actor.

### Weapons: binding an actor to a bone

- **`ModelBindInfo`** (vtable `0x1d1cab8`; constructor `0x1096f68`) keeps an actor on a bone of another actor. The recipe, from `0x96b28c`:
  - construct
  - `ActorLinkConstDataAccess::acquire(info + 8, parent, false)` at `0x1517e60`
  - bone name (`const char*`) at `+0x28`; unit/bone index (u16 each) at `+0x60`, set to `0xffff` so it's looked up by name
  - offset matrix at `+0x68` (identity by default)
  - `Actor::setModelBindInfo(actor, info)` at `0x1533750` (stores it at `Actor+0x4d0`); `0x1533760` clears it
- **Setting it isn't enough.** The binder calls `calc(info, child)` at `0x1097300` every frame (weapons' own AI does this). It resolves the bone, gets its world matrix from the parent's model (unit vtable `+0x70`), applies the offset and writes the child's `mMtx`. Called with a scratch buffer as the "child", it gives any bone's world matrix.
  - Checked: a Master Sword actor bound to Link's `Head` sat 1.5 m up, sticking out of his head.
- Link's attachment bones: `Weapon_R`, `Weapon_L`, `Pod_A`, `Pod_B`, `Pod_C` (back holsters), `Spine_2`, `Waist`.
  - Worn sword, shield and bow sit on `Pod_A`, the quiver on `Pod_B`, with offsets of up to 0.2 m.
  - Their own bind info names `Pod_A` but stays unresolved (`ffff`), so the game positions them another way (`Actor+0x4d8`, `0x1096d24`). The mod measures each weapon against the bones instead.
- Weapon actor packs each hold one model list. 266 weapons use 265 models.

### Trading

- A thrown or dropped weapon keeps its actor name and leaves the bone it sat on. The mod treats a `Weapon_*` actor more than 35 cm from all of Link's bones, with the name of something Link carried a moment ago, as let go.
- Moving such an actor away doesn't remove it: its physics puts it back. The mod kills it with `killWithDropsAndEffects(0)` (vtable `+0x1b0`) and never hands the same actor over twice.
- Spawning a real `Weapon_*` actor with `createActor` (`@P`, `@R`) gives a normal world weapon you can pick up.
- The Master Sword can't be thrown; R fires `PlayerBeam` and the sword stays on Link.
- The inventory's options for a material are only Hold and Cancel (Drop exists for weapons, bows and shields). Holding spawns the item actors as the menu closes, about 1.1 m above Link's feet in his hands. A puts them down or throws them; B puts them back in the bag (the actors go away).
- A weapon dropped from the inventory is spawned by Link while the menu is still open.
- The pause menu stops the world, and with it the clock: `WM_Time` doesn't change while it's open (our tick, driven by `getPlayerPos` callers, keeps running). In the System tab the clock runs. In play `WM_Time` also stands still for up to 2 s now and then, mostly in the first 10 s after a load, so the mod only counts a stop of 1.2 s or more.
- **A stopped clock isn't always the menu.** `TimeMgr::calc_` pins it in play: at 05:15 in a new game until the flag `IsPlayed_Demo103_0` (or `Demo997_0`) is set, which is when Link leaves the Shrine of Resurrection; at 11:00 until `FindDungeon_Activated` (the first tower); and at a fixed hour in its forced modes. Found on a real Switch with a new game: the mod took the whole start of the game for the menu, so no partner was spawned and no banner shown. While an event runs (talking, cutscenes) the clock is left as it is.
- **How the mod tells them apart** (`items.cpp`, `WatchPause`): when the clock has stood still for 150 ms it writes `WM_Time + 0.002`. `calc_` reads the flag, applies its rules and writes it back every frame, so a running world replaces our value within a frame (with the pinned hour, or with the time moved on); a paused one leaves it. A write shows up in what `getF32` returns one frame later (measured: 29 to 110 ms), not at once, so "ours never showed within 350 ms" also counts as the world running. The time sync's own write is used as the same kind of probe. `test_research = 5` logs every probe and the reads after it.
- Callers of `PlayerInfo::getPlayerPos` don't tell play from the menu: the same six call sites (return addresses `main+0xc3c1f4`, `0xc5a4e8`, `0xb312fc`, `0xb31418`, `0xb2fb14`, `0xb3233c`) call it once a frame in play, in the pause menu and in cutscenes (`test_research = 4` counts callers). A seventh seen only in play in the Shrine of Resurrection (`main+0xa87190`) belongs to an actor in that room.
- A guest whose clock is pinned can't follow the host's time: the game puts its own hour back a frame after each sync. The mod notices (same value again within 5 s) and stops trying for 15 s at a time.
- A guest's time sync writes `WM_Time` while the menu is open (every 10 s, since the host's clock keeps going), which looks like the clock moved. Between two real games that made a meal eaten just after it look like a heal outside the menu. The sync now tells the menu watcher, which ignores clock changes for 0.7 s after it.
- In the menu, the D-pad moves the cursor one slot; holding the left stick scrolls and speeds up; R/L switch between Adventure Log, Inventory and System. Moving right past a category's last column goes to the next category.
- A jump in Link's position (a warp, or fast travel within the overworld, which doesn't change the area) is treated like a load: everything around the new spot is new, and right after the first warp three wild animals there (two lizards and a frog, all `Animal_Insect_*`) were taken for things Link had dropped and "given" to the partner. Item tracking now restarts, with the 10 s quiet time, after a move of more than 40 m between two scans (200 ms apart).

### Horses

- `GameRomHorse01`'s pack: model list `EngineRom_Horse_Link`, ASList `Horse_Link` (AddRes `Horse_Link_Animation` retargeted to `Skeleton`), sequences `Wait`, `Move_Gear1`–`Move_Gear4`, `Move_Back`, `Jump_Gear3`/`4`, and others. Copied onto the monk NPC setup with no physics (`CoopHorse`), it stands and animates.
- Where the rider sits: `Model/Horse_Link.sbfres`'s skeleton has a `Saddle_Root` bone (parent `Spine_1`) at (0, 2.10, 0.05) m from the horse's origin in the rest pose. Link's riding clips (`Horse_Wait`, `Horse_Move_Gear_1`–`3`) keep his hips (`Skl_Root`) within 5 cm of his origin, so a mounted Link's position is the saddle point. The mod puts a rider's horse 2.10 m below and 0.05 m behind them. (Not measured on a real mount: a `GameRomHorse01` spawned on the plateau edge didn't appear.)
- The `A_Pose_Horse_*` clips are partial poses; a puppet playing one alone stands upright.
- `ASList::startAnimation`'s result can't be trusted either way: it returned false for sequences that visibly played (`Wait`, `Down`, and always for `Run`), and asking again restarts the clip from its first frame. So a sequence is started once per change, plus a couple of extra times in the first seconds after an actor is made (when it can ignore the call).
- **Body and clothes in step.** A partner is a bare body (`CoopPlayer`) with up to three clothing models (`CoopGear_*`) on top, each a separate actor playing the same sequence. The clothing code waited for a "yes" from the start call, never got one for `Run`, and so restarted the clothes' run on every frame: the clothes stayed on the first frames of the stride while the body ran on, and a running partner looked like two Links, a dressed one and a bare one, moving at different times (reported from a real Switch; standing still hid it). Now whenever a piece needs to start the body's sequence, the body and all pieces start it together (`DriveGear`), and the body's own early retries restart the pieces with it. The partner's horse had the same wait and was changed the same way.

### Starting sequences: what the call's result means (nothing)

- `ASList::startAnimationMaybe` (`0x149f24c`) returns nothing useful. Read as a bool it was 0 for sequences that visibly played, on the puppet (`Wait` in 60 test runs, `Down`) and on the real Link (`DamageL`, `SitDownSt`), and 5 for one that showed nothing (`SwimDead`).
- The puppet code used to ask again every 200 ms until the call said yes. For those sequences that restarted the clip five times a second: an idle that never got past its first frames, and a collapse that never fell. Now a sequence is started once when it changes; a puppet created less than 5 s ago is asked three more times, since a brand-new actor can ignore the call.

### Link while down

- Link's own state machine puts its sequence back within a frame when another one is started on his body slot (it re-requested `WaitAttention` right after ours). Dropping its requests for slots 0 and 1 in the `startAnimationMaybe` hook keeps ours; afterwards, starting `Wait` and letting requests through again brought him back, and he walked normally.
- **A dropped request can be one his state machine is waiting on.** After a lethal fall the game asks for `LandDamageStand` (getting up) and stays in that state until it has played. With that request dropped and `Wait` started instead, Link stood there for good: the menu opened, but he couldn't walk, jump or attack (seen in the recomp, after a revive from a fall). The hook now remembers the last dropped request for the body and the upper body, and the hold ends by playing it; he gets up and moves. `Wait` is only started when nothing was dropped.
- Of Link's sequences tried as a "down" pose: `Dead`, `DeadRagdoll` and `SwimDead` change nothing (they seem to need him really dying); `DamageL` and `LandDamage` are short crouches; `SitDownSt` sits him on the ground. The sit doesn't survive an enemy that keeps hitting him: hit reactions put him back on his feet by some other path.
- On the puppet, the one-clip sequence `Nml_Die` plays Link's collapse and holds its last frame, lying on the ground.
- The game's own callers pass `(-1, -1, name, slot, bank 0, true)`, the same as the mod.

### Progress flags and other players' stories

- `Clear_Remains{Water,Fire,Wind,Electric}` are single flags; nothing else named `Remains*` holds a beast's story (the rest are locations, small keys and the Fire beast's drone). The region's quest state lives in quest flags that aren't shared, so a clear flag alone gives a partner the ability and a "freed" beast in a world whose quest never ran. The mod applies it only to players inside the beast with the one who freed it.
- The Great Plateau shrines hand out the runes (`IsGet_Obj_Magnetglove`, `IsGet_Obj_RemoteBomb*`, `IsGet_Obj_StopTimer`, `IsGet_Obj_IceMaker`), and the Old Man's quest follows their clear flags. `IsGet_PlayerStole2` (the paraglider) marks the end of that tutorial; no partner progress is applied before it.
- Not guarded: `Clear_Dungeon120`-`136` (The Champions' Ballad shrines) are shared like any shrine, so a step of that quest can complete from a partner's clear.

### Horse coats

- Real horses can be spawned in Tabantha with `createActor` (`GameRomHorse05`, `GameRomHorse12` and `GameRomHorseEpona` all appeared; one kicked Link over).
- `GameRomHorse01`-`23`, `GameRomHorseEpona` and `GameRomHorseZelda` have byte-identical model lists (`EngineRom_Horse_Link`, no `VariationMatAnim`), the same `Horse_Link` ASList, and parameter lists that differ only in stats (life, gear charges, `ASVariation`, `RiddenAnimalType`). Own models: `GameRomHorse00L` (giant), `00S` (donkey), `Bone` (Stalhorse), `Nushi` (Lord of the Mountain).
- `Horse_Link.sbfres` has material animations `Horse_Link_Collar_Docile_fsp`/`_ftp`, `_Touchy_fsp`/`_ftp`, `_Epona_fsp`, `_Zelda_fsp`. On spawn the game starts `Collar_Shader_Impatient` (both normal horses tried) or `Collar_Shader_EponaHorse` on the horse's ASList, slot 0, bank 4, and `Collar_Impatient` / `Collar_EponaHorse` on a second list (the mane). The floats passed are -1, -1: the frame, which is the coat, is set some other way. `Horse_CollarType` (string at `0x15cc0a4`) is the likely source.

### Weapon bonuses

- InstParamPack value types: 3 int, 4 Vec3, 5 string (the decomp's enum order).
- `SharpWeaponAddType` and `SharpWeaponAddValue` are referenced only by two AI-action parameter loaders (`0x646924`, `0x650f68`). Where a weapon actor keeps its bonus wasn't found.

### Blood Moon

- `TimeMgr` adds to `WM_BloodyMoonTimer` (f32); past 7 days it sets `WM_BloodyDay`, and on a bloody day the Blood Moon rises at midnight. Both are program-writable game data. Guests copy the host's values once a second.

### Map reveal

- The map's revealed regions follow `MapTower_NN` (shared). `MapTower_NN_OpenScaleLevel` / `_OpenCenterPos` drive the reveal animation. Checked with a save whose 15 tower flags were cleared: dark map without a partner, fully revealed after receiving the flags.

### Weather

- `ksys::world::Manager` instance is at GOT `0x1d66460` (from the weather event action at `0x651a78`). Its fields match the decomp:
  - `mWeatherTypeTimer` at `+0x794`
  - forced `mWeatherType` at `+0x7d1` (`0xff` = not forced)
  - `mCurrentClimate` at `+0x780`
  - `mMgrs` (9 jobs) at `+0x5c0`, found at runtime
- `WeatherMgr` = `mMgrs[3]`; its current natural weather is at `+0x28`.
- **`Manager::setWeatherType(u32 type, bool, bool, bool for_demo)` is at `0x14315dc`.** Found through the weather name table at `0x1db1770`, which the string overload at `0x143162c` loops over.
  - It holds the weather for 4 frames, so guests call it every tick.
  - Checked: a guest's weather read 0 (clear), then 6 (thunderstorm) after the host's weather arrived, and the sky changed.

### Moving Link (warp)

- **What works:** Link's Havok position, `[[[main+0x1dbfdf0]+0xf0]+0xd28]+0x5e0` (Vec3f), written together with his actor matrix (`Actor+0x398`). This is the same chain as 1.6.0's `[[[main+0x2D37AF0]+0xF0]+0xD28]+0x5E0`, found by Bloom for the [botw-save-state](https://github.com/Pistonight/botw-save-state) practice mod; the struct offsets carried over, so a scan of 1.9.0's globals for a pointer leading there found the new global. Writing it moved Link 5 m, and he stayed.
- Also written: `PlayerInfo+0x88` (its position copy), the fall-start height `Player+0x20d0` (read 374.27 standing at y 374.27, so the 1.6.0 offset holds; without it a warp down counts as a fall), and the Havok linear velocity at position `+0xb0`. Held in the air, Havok keeps adding fall speed (y sat at the -64 m/s cap), and Link landed with it: from 120 to about 20 life.
- **Distance:** 15 m, 500 m and 1.5 km warps load in under a few seconds. While an area loads, the game holds Link perfectly still with the loading icon up; once he moves, it's loaded. At 4 km, and at 2 km in two 1 km hops, the area never loaded (30 s and more). The stage's load centre, `main+0x1e1289c` (set only when a stage loads, by `0xc5a478`, from `0xc465ac`), stayed at the start; forcing it to the new spot made the game stop holding Link, and he fell through ground that wasn't there. In 250 m hops, 4.2 km loaded hop after hop. The mod hops at y 1,100 (above every peak); up there the cold costs a little health, which the mod gives back.
- Moving every other copy of the old position (311 of them, found by a memory scan) didn't help either.
- **Finding that body:** the global chain above goes through a slot table (`+0xd28`) filled in load order. On a console and in Ryujinx the order never changes; in Savage Wind Recomp it does, and in one run of two the slot was empty, so every warp failed without a word. Link's body is also reachable from his actor: `[[[Actor+0x578]+0x80]+0x410]` is the `hkpRigidBody` (chain from Savage Wind's own teleport, `runtime/control_cells.c`). `LinkRigidBody()` (`player.cpp`) tries that first, checks the vtable, and falls back to the global chain; in Ryujinx both give the same address.
- **A far hop pauses the world.** While the map around Link isn't loaded the game stops the world (and the clock) every update, so a hop's load reads as "paused" to `WatchPause`: 1 to 4 s per slow hop, 17 s in a row in a busy Ryujinx. Nothing may be spawned then (correct), but two things went wrong with it: a partner's armor and weapons that arrived in that window were given up on for good (now they wait), and Link's hearts reading 0 for a moment around the warp and then full again counted as a meal eaten in the menu and was shared (now a rise from 0, or anything during a warp, isn't a meal).
- **Where a warp lands.** It was 2 m behind the partner, 0.5 m above their feet. On a slope steeper than 14 degrees that point is inside the hill, and with the partner's back to a wall it's inside the wall. Two test players set to follow each other (`test_follow`) on the Tabantha hillside ended up under the ground overnight, both of them, with the camera looking at the terrain from below; one was stuck there and no further warp moved him (`hop 1 didn't take`). A warp now lands on the partner's own position, 0.5 m up: the one place known to be free (puppets don't collide with Link). `WatchLanding` also puts Link back on the partner's spot if he drops more than 4 m below it in the 5 s after landing while the partner stays level (ground that wasn't solid yet), three times at most.
- **Fall damage is the drop from the highest point since Link left the ground.** The player actor keeps that height twice, at `+0x860` and `+0x20d0` (found by the Savage Wind Recomp project; its `teleport` resets them for the same reason). A warp's sky hops leave it at 1,100 m, so the landing counts as a fall of several hundred metres: 24 hearts gone on one arrival in the recomp, and a player with fewer than that went straight down (or to Game Over with nobody near). `Place` only reset the second copy, once, and the mod put lost hearts back for 3 s after arriving; that hid the problem wherever the last area loads fast and didn't where it loads slowly. Now both heights are set to Link's own height on every tick of a warp and until the guard ends, the guard counts from when the world runs again (8 s after a far warp: the cold of the sky still bit once after 3 s), and `tryRevive` gives his starting hearts back and returns true during that time, so a landing can never count as a death. Checked in the recomp: seven far warps in hops (up to 31 hops) with the hearts unchanged.
- **The game's question "is the ground around Link still loading?"** is `main+0x10b7a40` (no arguments, bit 0 of the result; found by the Savage Wind Recomp project). Its only caller (`0xc43cbc`) raises pause reason 9 while the answer is yes: that is the loading icon with Link held still. The call is skipped while either of two UI screens is up (`0x13f6990`) or while a flag is set that is cleared when the old stage's PlacementMgr has finished shutting down (`0x127c730`, cleared at `0xc40c6c`), so for a while after a load the game doesn't ask at all, and hops go through without a pause. The mod hooks the function (`GroundLoadingHook`): it counts the real answers, and can tell the caller "ready" (the gate).
- **Why far warps freeze.** Once the game is asking, a hop in the sky can stay "loading" for good: the world is paused, and going on, going back to the last hop that loaded, or waiting 2 minutes changes nothing. Seen again and again in Ryujinx on 2026-11-11, with the released build as well, on routes of 3 to 6 km (the same routes went through in the first 20 s after a save loaded, when the game wasn't asking yet). Slower hops (1.5 s each) didn't help. With the gate on in the sky the world ran but the answer still didn't turn to "ready" within 45 s in most tries.
- **One jump with the gate (`warp_direct = true`).** Link is put on the partner's spot at once and held there with the gate on; the world runs and loads around him; he's let go after 12 "ready" answers in a row (or 25 s with the game silent for 8 s), and the gate stays on until the answer has been "ready" for 4 s (it can turn back to "loading" as the map comes in by stages; without that the world froze with Link in a T-pose). In Savage Wind Recomp this is reliable: 15 of 15 warps of 2 to 9 km, 8 to 20 s each, hearts unchanged. In Ryujinx it worked about half the time (4 to 45 s); otherwise the answer stayed "loading" for the full 2 minutes and Link fell through ground that wasn't there. So hops stay the default, and a far warp that works everywhere still needs the game's own stage change (see REMAINING 5.3).
- **Landing where the ground comes late.** A shrine's platform, and the hollow in the hill it sits in, became solid 2 to 9 s after the ground around them: Link was first lifted 4 m (onto the hill that wasn't cut out yet), then dropped 1.9 m through the platform. `WatchLanding` now looks for 20 s, puts him back on the partner's spot when he's more than 1 m below or 4 m above it, holds him there for 1.2 s, and tries up to 12 times, 2 s apart; a partner who is moving at that moment no longer ends the watch. On the shrine platform the sixth try held.
- **Nothing is spawned while a warp loads the map** (`warp::Loading`): during the warp and 5 s after. The clock test alone doesn't cover it, because under the gate the world isn't paused.
- **A warp into Korok Forest hangs Ryujinx** (the game's main thread stops a few seconds after arriving at (418, 258, -2178); three times out of three, and with the released build too). Not looked into yet. The recomp doesn't hang there.
- A warp that starts in the first seconds after a save loads (only the `test_follow` option does that) doesn't take: the placement is written six times and Link stays put, then the next try works.
- Earlier, without the Havok chain: a breadth-first scan from Link's actor and his physics (`Actor+0x578`) found about 60 copies of his position, and raising them by 30 m didn't move him. `DynStartPos` and `StartPosName` are AI and debug parameters. Fast travel goes through the map screen (`0xdb155c` sets `App_Map_WarpLocationName`, setter `0xd35870`, then UI screen `0x3e`) and an event.

### The game's own stage change (travel)

- **`main+0x12082e0`** is the stage change: `void (const sead::SafeString& stage, const sead::SafeString& start_pos, int, bool)`. It calls a function pointer kept at `main+0x1e3f520` if one is set. Found from the event action that takes `WarpDestMapName` and `WarpDestPosName` (its parameters are read at `main+0x616de0`; its `enter`, at `main+0x616d44`, sets the fade from `FadeType` and tail-calls this with the two strings, the int at `this+0x48` and `false`). Calling it with `("MainField/E-4", "StartPos_CDungeon089", 0, false)` from the mod's tick brought up the loading screen and put Link on that shrine's travel pad, in Savage Wind Recomp (10 s) and in Ryujinx (22 to 40 s).
- **The names** are the map files' own. `Map/MainField/Static.smubin` (in `Bootup.pack`) has a `LocationMarker` list: 154 entries with `WarpDestMapName` (`MainField/<section>`), `WarpDestPosName` (`StartPos_CDungeonNNN`, `StartPos_TowerNN`, `HatenoVillage`, `CokiriVillage`...), a `SaveFlag` and a position. Only the shrines, towers, the two labs and the Shrine of Resurrection (138 in the overworld) are real travel points: the entries for villages name destinations that aren't theirs (Kakariko's is `MainField/F-2`, `StartPos000`), and a stage change to one of those put Link at the map's origin, (0, 98, 0). These are what the map screen's "Travel" uses (it stores the chosen one in `App_Map_WarpLocationName`, `main+0xdb16b4`, then opens screen `0x3e`). A section name is also what `main+0x1208344` makes from a position: `'A' + (x + 5000) / 1000`, `'1' + (z + 4000) / 1000`.
- **It's dropped while the world is settling.** In Ryujinx, a request made in the first 20 s after a save loaded did nothing (the game's clock was stopping and starting in that time). A request made once the world had run for a few seconds without a stop was taken.
- **Two requests close together break it.** Two calls 1 ms apart (a retry that fired twice) left Ryujinx on the new stage with the world stopped: Link's actor at the new place, `PlayerInfo`'s copy of his position still at the old one, and warps answering "didn't load". The mod asks once, waits 10 s for a loading screen, and only then asks again.
- **Ask from the main thread.** The mod's tick runs on whichever game thread asks for Link's position first in a frame, usually `MainThread`, sometimes a worker. Requests made from any thread left Ryujinx on the loading screen for good 3 times in 6; made only from `MainThread` (checked with `nn::os::GetThreadNamePointer(GetCurrentThread())`), once in 12. While such a load hangs, the world runs (the clock moves, about 110 actors exist) but the game never asks whether the ground is loaded; asking for the same stage change again didn't free it.
- **The mod stands back while its own stage change loads** (`warp::Traveling`): Link's actor stays through it, so the usual "no player, wait" rule doesn't apply, and the mod would otherwise go on touching puppets and enemies the game is tearing down. The enemy animation hook also only trusts its table when it was written in the last 0.4 s.
- **During the load, `PlayerInfo`'s position copy is stale**; Link's actor matrix is right. The mod calls the stage loaded when the world runs again, the two agree, and Link is near the travel point.
- Not tried: a shrine as the destination (`CDungeon/DungeonNNN` with `Entrance_1`: the code is there, from the names the shrine elevators use), and Korok Forest (`CokiriVillage`), where a plain warp hangs Ryujinx.

### Enemy bodies (enemy movement sync)

- Link's Havok position is inside an `hkpRigidBody`: its vtable is 0x1c0 before the position, at main+0x1b87b20, and the `hkpMotion` starts at body+0x150 (position at body+0x1c0, linear velocity at body+0x270). Enemies' bodies have the same vtable, and a Bokoblin's body sits exactly at its actor's position.
- No pointer path from an actor, or from its physics (`Actor+0x578`), to its body turned up, 3 or 4 levels deep. The mod finds a body by scanning memory for the vtable at the actor's position: a full scan of the address space took 17-20 ms, and the regions where bodies turn up are remembered and scanned first.
- Writing the body position (and the actor matrix) moved a Bokoblin 4 m, and it carried on from there. Writing its velocity did nothing: its AI sets that every frame. So the mod slides the body toward the owner's position each tick (at most 12 m/s, horizontal only) and snaps it when it's more than 4 m off.
- **Only characters have a body of this class.** Every body with that vtable within 30 m of a Bokoblin camp was Link's or a Bokoblin's: no rocks, weapons or trees. So the nearest free body within 2 m of an enemy (3 m in height) is taken as its own, and the offset from the actor is kept for enemies whose body isn't at their feet.
- **A sleeping Stone Talus has none.** `Enemy_Golem_Senior` (hash `132fec6f`, at (-2864.5, 409.0, -1825.9)) and its part actors (`Enemy_Golem_Upper_Arm_L`, hash 0) had no such body within 30 m while dormant. Not checked awake.
- A full scan of the address space that can't stop early takes about 170 ms (stopping at the first exact match took 17-20 ms), so full scans are limited to one every 30 s once any body has been found; the remembered regions are scanned otherwise.
- Height: a copy 5 m above or below its owner's is moved in height too (past 0.75 m of difference; less than that is left to the ground).
- Warp detail found on the way: right after a placement, Link's position can read as the old spot for a tick, which looked like "he moved, so the area has loaded". The warp now judges a hop only once Link reads as being on it.

### Save files

- BotW mounts its save data as `save` (`nn::fs::MountSaveData`) and names files through its own file layer (`%s://%s`): `save:/0/game_data.sav` … `save:/7/`, `caption.sav`/`caption.jpg` per slot, `option.sav`, `tracker/trackblock%02d.sav`, `pict_book/*.jpg`, `album/`, `exhibition`. Every access ends in an imported `nn::fs` call that takes a path: `OpenFile`, `CreateFile`, `DeleteFile`, `GetEntryType`, `OpenDirectory`, `CreateDirectory`, `DeleteDirectoryRecursively`. Hooking those and rewriting `save:/X` to `save:/coop/X` moves all of it; `CommitSaveData("save")` commits the whole file system, so nothing else changes.
- `nn::fs::DirectoryEntry`: the entry type (0 directory, 1 file) is the byte at +0x304; the header's `m_Type` at +0x301 is padding. Reading it from +0x301 made directories look like files, so the first copy failed and the game, finding no saves, started a new game. The mod now uses only a complete copy.
- A 100% save is 226 files (the picture book alone is over 150 photos).

### On-screen messages

- The info banner: `0xe7a780(idx)` shows MainScreen_00 info message `idx` (it loads the empty string and falls into `0xe7a790(idx, const SafeString*)`, which gets the MainScreen from `0xe7a240` and queues the message with `0xda9df4`). The labels are a table of 0x31 SafeStrings at `0x1daa3b0`: index `0x21` is "0028" ("The Master Sword has returned to the forest"), `0x2a` is "0061", the same two the 1.6.0 practice mod borrows.
- **The banner is one short line.** Measured in the recomp at 1280x720: the text pane is about 470 px wide, which is 48 characters of ordinary text (36 capital W's). Text that fits is centred. Text that doesn't is cut off on the left, and anything past the 56th character is never drawn at all. The game's size tag (`0x0e, 0, 2, 2, percent` in the UTF-16 text) does shrink the letters, but the line is still placed as if it were full size, so a shrunk long line loses its start just the same. So every banner is written to stay under about 46 characters, with partner names cut to 10 letters in the longer ones.
- Text comes from `getMessage(const SafeString& file, const SafeString& label, out)` at `0xe84870` (0 = found; out = UTF-16 pointer and u32 length). Hooking it for `LayoutMsg/MainScreen_00` with those two labels, for 2 s after showing one, swaps in our text; the game's own messages are untouched.
- Item names: `getMessage("ActorType/Item", "<Actor>_Name")` gives "Apple" for `Item_Fruit_A`. Weapons are in `ActorType/WeaponSmallSword`, `WeaponLargeSword`, `WeaponSpear`, `WeaponBow` and `WeaponShield`; insects and bee hives in `ActorType/CapturedActor`.
- sead::SafeString's vtable: GOT slot `0x1d66260`, +0x10.

### Controls the mod takes

- Clicking the right stick opens the Scope, and D-pad Down whistles (calling the horse, with its own banner), so neither works as a co-op button. The mod keeps the left stick click from the game while it's held: released within 350 ms, it's passed on as a tap (Link crouches); held longer, it's the mod's.

### Loading screens with a partner connected

- **Never create an actor during a loading screen.** This was known for the first load; it's just as true for fast travel and shrines. The game removes the mod's actors when a load starts, and the mod used to make the puppet again at once: the load then never finished (136 respawns in 45 s, with one game and a fake partner). Until this was found, nobody had fast-traveled with a partner connected.
- The signal: the game's clock (`WM_Time`) stands still for the whole load. Actors are only created once the clock has run for 1.5 s, and not within 3 s of the game removing one of ours (which is how a load starts, before the clock stop is noticed). Gifts wait the same way.
- **What's attached to the puppet dies with it.** Armor, weapon and horse actors are removed by the same load, and their pointers were kept and written to every frame. One run crashed inside `startAnimationMaybe` on a freed armor actor (`PC main+0x149f250`, called from the mod). They're now forgotten with the body, and checked against the live actor list every frame.
- Armor and weapons are only kept on a puppet within 120 m (160 m to take them off): nobody can see them farther away, and the fewer actors that have to survive far from Link, the better.
- A second Ryujinx on the same Mac did not hang fast-travel or shrine loads; that quirk seems to be about the first load only.
- **A Ryujinx window that is completely covered stops.** With two recomp windows (1280x748) lying over the test copy's Ryujinx window, the Ryujinx game sat at 0.00 FPS with its log still ticking, hops "didn't load", and the relay dropped it for not answering; brought to the front, it ran again at once. A window that is only partly covered keeps running. The recomp test copies are now started without a window (`SAVAGEWIND_HIDDEN=1`; screenshots through the control port still work). This may also be part of the "second Ryujinx hangs on the loading screen" quirk when two Ryujinx windows open on top of each other; not checked.
- With two real games: player 2 fast-traveled 648 m (Vah Medoh's travel point), player 1 saw the banner, warped over in 3 hops and arrived 2 m away; player 2 walked into Dunba Taag Shrine (`CDungeon / Dungeon055`) with a partner connected.

### Enemy life before an enemy wakes

- Many placed enemies read life 1 with a max of 48 to 864 until they become active (72 of them around the Tabantha camps). Life going from that 1 up to the real value is not a heal, and "less than max" doesn't mean hurt: only an enemy whose life has been seen to go down is treated as hurt.

### Game over and reload

- With `tryRevive` returning false the game shows Game Over; Continue reloads the last save in place. The game removes the mod's actors as in any load. The flag tables' reset counter (`Manager+0xc28`) does not change on this reload, so it isn't seen as a new save: the mod has to compare what it believes is set against the game to notice the rewind.

### Carried weapons: three sources of churn

- A bow on Link's back sits about as close to `Spine_2` as to `Pod_A`. An idle animation flipped the nearest bone about once a second; each flip sent a new message and re-bound the weapon on every partner's puppet. A weapon now stays on its last bone unless another is under half the distance.
- Now and then a scan finds no weapon on any bone (the bone matrices aren't there for a moment), and during a warp's hops the weapons trail Link. Sending "no weapons" made partners take everything off the puppet and spawn it again; fewer weapons than last time now has to hold for 0.75 s.
- With three players standing together (two of them with a full set of gear), no bone of Link's could be read (`ModelBindInfo` calc returns false for all seven) for over a second at a time, every 5 to 10 s, in Ryujinx and in the recomp alike; with two players it didn't happen in five minutes. The 0.75 s rule above didn't cover it, and both geared players' weapons were taken off and remade on every screen every few seconds. A scan in which no bone can be read is now thrown away. Why the skeleton goes missing isn't known.
- `Item_Conductor` is an actor that can appear beside Link by itself; it isn't something carried, and is ignored by the trading code.

### The map screen, for test scripts

- Minus needs a 300 ms press to open the map. The cursor starts on Link; the left stick moves it and it snaps to icons. From the Tabantha test spot, 220 ms left lands on Vah Medoh's travel point and 150 ms down on Dunba Taag Shrine. A, then A on "Travel", travels.
- After traveling to a shrine, Link stands on the pad facing the camera with the shrine behind him: stick up for 1.6 s walks him in, and the lift takes him down without a prompt.

### Module pitfalls

- **No function-local `static` with a runtime initialiser.** It compiles to `__cxa_guard_acquire`, imported from the SDK. Called inside the game's frame (from a hook), it hung the whole game. Use file-scope variables instead. `nm -u client.elf | grep guard` should print nothing.

### Things that turned out to be wrong

- **"A player actor exists" doesn't mean "in the world".** The title screen renders the Great Plateau with a Link actor parked at exactly `(-1060, 250, 1830)`. The mod treats that exact position as the title screen.

- **`PlayerInfo::mPlayerPos` is `(0,0,0)` until the world has loaded.** The player actor already sits at the save position during the loading screen, but `mPlayerPos` only becomes non-zero once gameplay starts. The mod uses that as its "in the world" signal. It reads the position and facing values themselves from the player actor's matrix (`PlayerInfo+0x60` → `Actor+0x398`).
- **Spawning actors during the loading screen stalls it.** Seven synchronous `createActor` calls mid-load left BotW on the loading screen indefinitely. Wait for the signal above.

### Enemy sequences (attack sync)

- Every sequence an enemy plays goes through the same call as Link's, `ASList::startAnimation` (main+0x149f24c), on the enemy's own list (`Actor+0x560`). For Bokoblins the arguments are always slot 0, both floats -1, bank 0, flag true; only the name changes.
- Names seen on Bokoblins. What the AI chooses: `Wait`, `BattleWait`, `Run`, `Walk`, `BattleWalk`, `BackWalk`, `SideWalk`, `Turn`, `Notice`, `Search`, `SearchCareFree`, `CareFree`, `Attack`, `DoubleAttack`, `AttackPursuit`, `AttackRotationEnd`, `JumpAttackStartQuick` / `JumpAttackMove` / `JumpAttack`, `NoWeaponAttack`, `GrabAndShoot`, `Catch`, `Guard`, `GuardWalk`, `GuardTurn`, `GuardSideWalk`, `GuardBackWalk`, `ArrowReload` / `ArrowDraw` / `ArrowShoot`, `BowTurn`, `BowBackWalk`, `BowBattleWalk`, `Signal*`, `Sleep`, `SleepCreate`, `SleepEnd`, `Squat`, `Talk`, `TalkStart`, `ListenStart`, `Dance`, `EatMeat`, `Angry`, `Hide`, `Noise`, `RaiseEye`, `Cold`. What happens to it: `SmallDamage`, `BlownOff`, `DownWait`, `DownRecover`, `Burn`, `BurnEnd`, `Burnward`, `Fall`, `Scrap`, `Die`.
- **An enemy whose sequence requests are dropped doesn't attack.** Its AI asks again about once a second for the same sequence (`Run`, `Catch`, `BattleWait`) and gets no further: with every request of three Bokoblins dropped, Link stood among them for 70 s without a hit. So a copy can be starved of its own AI's sequences and fed its owner's, and there are no hits from an enemy that isn't swinging on screen. An archer's AI does go through `ArrowReload`, `ArrowDraw`, `ArrowShoot` by itself on a timer; no arrow was seen from a starved one, but that was only watched for a short time.
- An enemy that never played anything stands in the T-pose. A starved copy therefore gets its AI's first request, and after that its owner repeats what each enemy is doing every 2 s, played only where the copy isn't doing it already.
- Swapping every request for `Wait` leaves the enemy standing in its idle pose next to Link: what the mod does around a downed player.
- A weapon actor's life (`getLife()`, as for enemies) is its durability in the inventory's units: the Master Sword read 7440, a Hylian Shield 79900.

### Player items and explosions (not solved)

- The actors with the profile `PlayerItem` are the runes' and the paraglider's: `RemoteBomb`, `RemoteBomb2`, `RemoteBombCube`, `RemoteBombCube2`, `PlayerStole2` (the paraglider, model `Item_Parastole2`), `Item_Magnetglove`, and the `Obj_*` rune entries. Created by the mod they exist (a `RemoteBombCube` lights the ground blue) but draw no model. Setting a spawned bomb's life to 0 does nothing.
- The blast is its own actor, `Explode` (profile `Bullet`). Created without parameters it sits there and does nothing. One of the game's own callers (main+0x726e60) builds its parameters as `AttackPower` (int, 0 there), `ScaleTime` (float, 1.0) and `Range` (float, 0 there), then calls `createActor` with the name held at `this+0xc0` ("Explode" by default). In the game's parameter pack an int is type 0 and a float type 2 (a vector 4, a string 5).
- `createActor` (main+0x1535900) has 111 callers; the mod's test console can log what the game creates and with which parameters (`probe`, development builds only).

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

### The Link puppet (`CoopPlayer`)

`CoopPlayer` is the monk's setup with Link's model and Link's animation sequences, and no physics. Its pack contains:
- the monk's AI (`DemoWaitNpc`, whose root action plays the sequence named `Wait`)
- `PauseMenuPlayer`'s model list (`Link`)
- a generated one-clip sequence (element type 67) playing `Nml_Wait` from `Player_Animation`
- `PhysicsUser`, `AttentionUser`, `ChemicalUser` and the e/s/x-link fields set to `Dummy`

**Result: Link draws, follows, and animates.** Two things stood in the way, and both looked like a T-pose:

1. **The ASList must be sorted by name.** The game looks sequence names up with a binary search (`main+0x14ffdb4`, reached via actor `+0x570` → `+0x98`). An unsorted `ASDefines` list makes some names silently unresolvable. My first list put `Wait` first, so it never played. `tools/build_romfs_mod.py` now sorts it.
2. `instSize` (the actor's own heap) is raised from the monk's 326,640 to 700,000; the pause-menu Link uses 557,256. It's unclear whether this was needed on its own, but it's the safe value.

How sequences are started (all in `offsets.hpp`):
- An actor's `ASList*` is at `Actor+0x560`.
- `ASList::startAnimationMaybe` is `main+0x149f24c`: `(list, f32, f32, const SafeString& name, int slot, int bank, bool)`. It returns nothing.
- It silently does nothing if any of these fail:
  - `list+0xd8` (the owning actor) is set
  - `slot < list+0xb8`
  - `bank < slots[slot]+0x20`, where the slots are at `list+0xc0`, 0x50 bytes each
  - the name lookup (`main+0x149ed3c`) finds it
- The mod calls it directly (`PlayAS` in `ghost.cpp`). It uses one-clip sequences (element type 67) for `Nml_Wait`, `Nml_Move_Walk`, `Nml_Move_Run`, `Nml_Move_Dash`, `Fall`, `Float_Float_F` (gliding), `Swim_Move_Slow_F` and `Swim_Wait`. Clip names come from `Player_Animation.sbfres`'s string table.

**The player's own animation state.** The mod hooks `ASList::startAnimationMaybe` and watches calls for the local player's list (player actor `+0x560`), on slot 0 (the main body).
- The player's sequence names come from `GameROMPlayer`'s ASList: 399 of them, such as `Wait`, `Move`, `ClimbMove`, `ClimbWait`, `SwimMove`, `SwimDash`, `ParashawlGlide`, `Jump`, `Fall` and `HorseRun*`.
- `game/anim.cpp` maps them to a small category sent in `PlayerState::anim_hash`. Slots 1 and 3 carry upper-body and face sequences.

Animation-slot layouts (keys are stored as hashes: `NumASSlot` = 1749500541, `IsParticalEnable` = 670993712, `TargetType` = 3788161224):

| Slot | Monk | Pause-menu Link |
|---|---|---|
| 0 | face (`Face_Root`), skeletal, 2 slots | whole skeleton, partial off |
| 1 | body (`Root`, face excluded), skeletal | `TargetType 1`, partial body |
| 2 | `TargetType 3` (`Root`, `Spine_1`, `Face_Root`) | `TargetType 2` |

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


## Running on a real console

First run on a real Switch on 2026-11-10: the mod loaded, connected over Wi-Fi and showed partners (see `docs/SWITCH.md` for what was and wasn't tried). What differs from Ryujinx, found by reading files before that run:

- **Permissions (`main.npdm`).** exlaunch's hooks patch the game's code through `svcMapProcessMemory` and get their own process handle with `svcCreateSession`/`svcReplyAndReceive`. A retail game's npdm doesn't allow those calls (BotW 1.9.0 allows 50 system calls, none above 0x5f); Ryujinx doesn't enforce the list, a console does. `client/config.json` builds an npdm for the mod. It used to carry exlaunch's sample title id and lacked five services the 1.9.0 game asks for (`ectx:aw`, `mnpp:app`, `ngc:u`, `ngct:u`, `notif:a`) and one flag (`disable_device_address_space_merge`: the game's flags byte is 0x27). It now matches the game's file in title id, services, flags, stack size and thread limits, and adds the system calls. Ryujinx loads it when it's in the mod's `exefs` ("main.npdm replaced") and the game runs; it also turns on SD card access there, which the game's own npdm refuses (0x320002).
- **Save data.** The save's header (`ExtraData0` in Ryujinx's save folder) gives a data size of 65,011,712 bytes and a journal of 1,572,864 bytes. Everything written between two `CommitSaveData` calls has to fit the journal on a console. A save slot's `game_data.sav` is 1,027,264 bytes, the biggest file there is. `saves.cpp` commits before the next file would take the running total past 1 MB.
- **Speed.** A full pass over memory for rigid bodies took 170 ms on an M4. `enemies.cpp` now searches 3 ms per frame and resumes.
- **Log.** `svcOutputDebugString` goes nowhere a player can see. `filelog.cpp` keeps the last 64 KB of log lines and, with `log_file = true`, the network thread appends them to `sd:/botw-coop/log.txt` every 2 s (`MountSdCardForDebug`, which needs the npdm above).


## Savage Wind Recomp (a native build of the game)

[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) recompiles BotW 1.9.0 to native code. It has no loader for Switch code, so the checkout at `~/SavageWindRecomp` carries local changes (not upstream) that take this mod as it is built for the console:

- `tools/mod_add.py <subsdk9> <client.elf>` puts the mod in `extracted/exefs`; the mod is recompiled with the game, and its exlaunch hook calls are answered by the runtime (`runtime/mods.c`). Game functions it hooks are listed in `extracted/exefs/hooks.txt`. Its sockets are the computer's.
- After every `make -C client` here, the game code there has to be built again: `tools/mac/mod_rebuild.sh` (4 to 8 minutes). Test copies: `tools/mac/coop.sh <n> <seconds>` starts copy n (settings in `build-vk/coop<n>/romfs/botw-coop/config.ini`), `PORT=780<n> tools/mac/ctl.sh <command>` drives it (`press`, `stick`, `shot`, `mem read`, `state`).
- It runs the same mod code, so it is a third platform to test on, and it found bugs the other two hid: the slot-table order (above), the gear that was never retried, the false meal, and the three-player weapon churn.
- **Two more, both from standing on a partner's very spot** (which is where a warp lands): the scan for Link's own weapons took every `Weapon_*` actor within reach of his bones, the puppet's included, so the list sent out changed on every scan (now the mod's own puppet weapons are left out, `IsPuppetWeapon`); and in Ryujinx single bones became unreadable for a scan, which moved the bow from `Pod_A` to `Spine_2` and back about once a second (now a scan is thrown away when a bone that carried something last time can't be read). Before: 100 weapon messages in 20 s; after: none in 30 s.
- What behaves differently there: far hops load slower (1 to 3 s each against 5 to 160 ms in Ryujinx), and load order varies from run to run.

## The inventory (pouch)

`PauseMenuDataMgr` (instance through the GOT slot in `offsets.hpp`) is laid out as in the 1.5.0 decomp. Checked by walking it at runtime (`test_research = 3`) on a 100% save: 320 items, names and bonuses matching the inventory screen.

- `+0x28` a `sead::CriticalSection` (its mutex at `+0x48` read `0x101`).
- `+0x68` the item list (`sead::OffsetList`: node `{prev, next}`, `s32 count`, `s32 node offset` = 8); `+0x80` a second list (free items).
- `+0x98` the 420 items themselves, `0x298` bytes each. The list's nodes always point into this array.
- An item: `+0x08` list node, `+0x18` type (0 sword, 1 bow, 2 arrow, 3 shield), `+0x20` value (durability x100 for weapons, count for stacks), `+0x24` equipped, `+0x30` name pointer (to its own buffer at `+0x3c`), `+0x80` bonus value, `+0x88` bonus flags.
- Bonus flags are the game's `WeaponModifier` bits: 1 Attack Up, 2 Durability Up, 4 Critical Hit, 8 Long Throw, 0x10 Multi-Shot (value = arrows), 0x20 Zoom, 0x40 Quick Shot, 0x80 Shield Surf Up, 0x100 Shield Guard Up; `0x80000000` marks the stronger (yellow) kind. Seen: Savage Lynel Crusher `0x80000001` / 44, Royal Claymore `0x80000002` / 16, a bow `0x80000010` / 5, a shield `0x80000100` / 20.
- `itemGet(mgr, name, value, modifier*)`: for a weapon `value` is the durability, and `modifier` is `{u32 flags; s32 value}`. The list count going up says it went in; it doesn't when the inventory is full.
- A weapon dropped from the menu stays in the list with value 0 until the menu closes, then leaves it.
- In the menu, a dropped two-handed weapon lands 2.7 m from Link.
