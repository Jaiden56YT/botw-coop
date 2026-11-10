# What's left to do

Everything that isn't finished yet, as of commit `4aa4de3`. It covers features from the design that don't exist, features that exist but haven't been checked properly, known bugs and rough edges, and the work needed to hand this to other players.

**Size** is a rough guess:
- **S** is an afternoon.
- **M** is a day or two.
- **L** is a week or more.
- **?** means it depends on reverse-engineering that hasn't been done, so nobody knows yet.

**Priority:**
- **P1** matters most for two people actually playing together.
- **P2** makes it clearly better.
- **P3** is nice to have.

---

## Contents

1. [Not checked well enough yet](#1-not-checked-well-enough-yet)
2. [Combat](#2-combat)
3. [Death and revival](#3-death-and-revival)
4. [Seeing each other](#4-seeing-each-other)
5. [Moving around together](#5-moving-around-together)
6. [Story, shrines and cutscenes](#6-story-shrines-and-cutscenes)
7. [Trading and items](#7-trading-and-items)
8. [Horses](#8-horses)
9. [Saves](#9-saves)
10. [Joining, settings and the session](#10-joining-settings-and-the-session)
11. [Networking](#11-networking)
12. [Performance and stability](#12-performance-and-stability)
13. [Known bugs and rough edges](#13-known-bugs-and-rough-edges)
14. [Shipping it to other people](#14-shipping-it-to-other-people)
15. [Suggested order](#15-suggested-order)

---

## 1. Not checked well enough yet

Several features were only tested against `tools/fakehost` (a scripted fake partner), not between two real games. A fake partner sends perfect data on time. Real games don't, so these need a proper run with two test copies (`P_DIR=.ryujinx-p1` plus player 2). They're **P1** because each one could be broken in a way nobody has seen yet.

| Feature | What was checked | What still needs checking |
|---|---|---|
| Shared meals | A fake partner's meals healed player 2, and player 2's meal reached the fake partner. | Two real games eating next to each other. Eating several dishes quickly. Hearty dishes (yellow hearts) when the partner is at full health. Elixirs. |
| Downed state | A fake partner walked over and got player 2 up, or stayed away until the time ran out. | A real partner walking over. Warping to a downed partner. Both players going down at once. Going down in water, mid-air, while gliding or in lava, where pinning Link in place might look wrong or kill him anyway. Going down inside a shrine. |
| Low-hearts warning | A fake partner whose hearts drop. | Real games. Also, it shouldn't fire on a fresh save with 3 hearts, or after a heart-container upgrade. |
| Travel banners | A fake partner jumping 1.5 km and walking into a shrine. | Real fast travel, which goes through a loading screen. Real shrine entry and exit. Divine Beasts. Hyrule Castle. Stalling on the loading screen shouldn't trigger false banners. |
| Separate co-op saves | Test copies with a 100% save: the first copy, a save from the menu, the second launch and the backup. | **Your own real save and profile.** A fresh save with no slots. Several Ryujinx user profiles. Master Mode saves (`tracker/trackblock_hard*`). Autosaves in many slots. What happens if the game is killed during the first copy. |
| Warp | 15 m, 500 m and 1.5 km warps, a 4.2 km warp in hops, and one warp between two real games. | Warping to a partner who is gliding, climbing, swimming, standing on a moving platform or inside a building. Warping while you're in a menu. Warping in the desert (heat) and on snow peaks (cold). Warping at night during a Blood Moon. Warping a long way in Hebra, where 1,100 m up may not clear every peak. |
| Enemy sync | Bokoblins at one camp, with two real games. | Big enemies (Hinox, Stone Talus, Lynel, Molduga, Guardians), whose rigid body may not sit at the actor's position, so the body search would fail. Flying and swimming enemies (Keese, Octoroks, Lizalfos in water). Enemies on horseback. Enemies in shrines and Divine Beasts. Ownership passing from one player to the other mid-fight. 20 or more enemies at once (one message holds 24). |
| Trading | Weapons and materials with a fake partner, and a menu drop. | Two real games. Weapons with bonuses (+attack, durability up), which currently arrive as plain weapons. The Master Sword. Throwing into water or lava. |
| 3–4 players | Player 2 plus two fake partners. | Three or four real games, for warp cycling, enemy ownership between more than two players, and banners. |
| Over the internet | Everything ran on one Mac (`127.0.0.1`). | Two Macs over Tailscale or ZeroTier, with real lag (50–150 ms). Enemy steering and warps were tuned with no lag at all. |

---

## 2. Combat

### 2.1 Enemy animations and attacks (P1, L, ?)
**Today:** only where an enemy is and which way it faces is shared. Each game still runs its own copy of the enemy's AI, so in your partner's game a Bokoblin can swing at a different moment, or at nothing.
**Wanted:** the owner's enemy is the real one, and everyone else's copy is a puppet that plays the same animations: attack, block, stagger, get knocked down.
**How:**
- Send the enemy's current animation the same way the player puppet does (it reads the `ASList` sequence names, see `anim.cpp`), and play it on the copies.
- Turn off the copies' AI. That's the hard part: the enemy AI is only partly decompiled.
- A middle step is to keep the AI running but override its animation choice every frame.

### 2.2 Hits from any player reach the owner, with their effects (P2, M)
**Today:** health is shared (`combat.cpp`): damage done in any game lowers the enemy's health everywhere. Fire, shock, freeze, knockback and stuns don't carry over.
**How:** send the kind of hit along with the new health, and apply the effect on the owner's copy. That needs the game's damage call, with its attack info, found in 1.9.0.

### 2.3 Rune and physics objects (P2, L, ?)
**Wanted:**
- Stasis on an enemy or object shows for everyone.
- Bombs, Magnesis and Cryonis pillars exist in both worlds.
- A boulder one player knocks loose rolls the same for the other.

**How:**
- Treat physics objects like enemies: the last player who touched one owns it and sends its position. The rigid-body trick from enemy sync can move them.
- Rune actors (`RemoteBomb`, `RemoteBombCube`, Cryonis blocks) need spawning on the partner's side, the same way gifts are.

### 2.4 Per-player difficulty (P3, S)
The design wants config settings for enemy health, enemy damage and stun time. For example, enemies could get more health with each player present.

### 2.5 Friendly fire and Rivals mode (P3, M)
- **Default:** off, and you can't hurt your partner, which is how it works today: the puppet can't be hit.
- **Rivals mode:** you can hit the puppet, and the hit is sent to its player as damage. That needs the puppet to take hits (a hit box on `CoopPlayer`).

### 2.6 Unplaced enemies (P2, M)
**Today:** only enemies placed on the map share health and position, because only those have the same `HashId` in every world. Random spawns don't: Blood Moon respawns, enemies that drop from the sky, Lynels at night, Guardian Scouts in some spots, and anything spawned by `test_spawn`.
**How:** give each spawned enemy a network id from its owner, and spawn it on the partner's side.

---

## 3. Death and revival

### 3.1 A real "down" look (P2, M)
**Today:** a downed player just stands still, held in place.
**Wanted:** Link on the ground or crawling, and the puppet showing that too.
**How:** play one of Link's own lying-down or damaged sequences while down (look through `Player_Animation.sbfres` for a suitable clip), and send a "down" movement state so the puppet plays it as well.

### 3.2 Reviving on purpose (P3, S)
**Today:** standing within 2.5 m of a downed partner for 1.5 s gets them up.
**Design:** hold **A** next to them for 3 s.
**How:** the input hook already sees every button. Note that A next to a downed partner might also pick something up or open something.

### 3.3 What happens when nobody comes (P2, M)
**Today:** you're released on a quarter heart, and the next hit is a normal game over, which reloads your save and drops you out of the shared state.
**Design:**
- Respawn next to your partner instead, weakened (less stamina for 60 s).
- If **both** players are down, the whole party respawns at the host's last safe spot, without reloading from disk.

**How:** the warp code can place you next to your partner. Avoiding the game-over screen entirely needs `tryRevive` to keep returning true and life to be restored.

### 3.4 A game over in co-op (P2, M)
**Today:** if you die alone, the game reloads your last save. The mod then sees a new stage and re-creates the puppets, but your world rewinds while your partner's doesn't.
**Check:**
- whether shared progress earned in between is caught up again (it should be: catch-up runs on load);
- whether enemy health from before the reload clashes with the partner's.

---

## 4. Seeing each other

### 4.1 Name tags over the puppet (P2, L, ?)
**Wanted:** the partner's name, and maybe their hearts, floating over their head.
**Problem:** BotW has no text-over-a-character feature to borrow.
**Options:**
- **(a) Draw text ourselves:** hook the render loop and draw with the game's font. That's hard: it needs the NVN graphics API, the font, and the camera.
- **(b) Reuse an existing marker:** something like the enemy "noticed you" marker, or a map pin. No name, but it shows where they are.
- **(c) Put the name in the banner** when you get close ("FakeA, 4 hearts"). Easy, and the banner already exists.

### 4.2 Hearts always on screen (P2, M, ?)
**Today:** a banner warns when a partner is low.
**Wanted:** a small always-visible line, like "FakeA ♥ 12/20".
**How:** the 1.6.0 practice mod uses the "tips" message box (`MessageTipsRunTime`, see `docs/reverse-engineering.md`, On-screen messages). Its 1.9.0 address hasn't been found yet, and the practice mod calls that method unreliable even in 1.6.0.

### 4.3 Partner on the minimap (P3, ?)
The partner is already a stamp on the full map. The design also wants them on the minimap in the corner of the screen, which is drawn by a different screen.

### 4.4 Shared map stamps, pins and beacons (P3, M)
**Wanted:** a stamp you place shows on your partner's map too, so you can say "meet here".
**How:** `map.cpp` can already add stamps (`MapMgr_AddStamp`), and the remaining work is noticing when a stamp is added or removed and sending that.
**Beacons** (the Sheikah Slate's pin) are a separate system.

### 4.5 More of what the partner is doing (P3, M each)
The puppet shows movement, attacks, the bow, throws and the shield. It doesn't show:
- the paraglider (it glides with empty hands);
- runes in use (bombs, the Sheikah Slate);
- holding something above the head (a material, a Korok's rock);
- eating, sitting, sleeping;
- climbing gear or other armor effects (the fire armor flame and so on);
- weapon elements (a flame sword's fire);
- damage reactions and getting knocked over.

### 4.6 Fade-out when a partner disconnects (P3, S)
**Design:** they fade out over 10 s.
**Today:** the puppet disappears at once.

---

## 5. Moving around together

### 5.1 Travel together (P2, M, ?)
**Design:** fast travel is a vote. When one player travels, the other gets "Travel together / Stay".
**Today:** a banner says your partner traveled and that holding L3 follows them. Following means warping across the map in hops, which takes about 1 s per km in the sky.
**How:**
- Ask with the banner, and answer with a button.
- The best version would use the game's own fast travel to the same named place. That runs through the map screen, an event and a loading screen, and is the stuck point recorded in `docs/reverse-engineering.md` under Moving Link (warp).

### 5.2 Warp across areas (P2, L, ?)
**Today:** warping only works inside the same area: both in the overworld, or both in the same shrine.
**Wanted:** warp into the shrine or Divine Beast your partner is in, or back out.
**How:** that needs a stage change: loading the shrine's map and placing Link at its entrance. No free-position stage-change call has been found yet. Shrine entrances are named start points, which might work with the same event mechanism fast travel uses.

### 5.3 A smoother long warp (P3, M)
**Today:**
- warps over 250 m jump through the sky in 250 m steps at 1,100 m up;
- you see the sky between steps;
- the cold up there costs a little health, which the mod gives back afterwards.

**Ideas:**
- Fade the screen to black during the hops.
- Lower the hop height where the terrain allows it.
- Find the world-streaming call so a single jump loads the destination directly. This is the real fix. Notes on what was tried are in the reverse-engineering doc.

### 5.4 Choosing who to warp to (P3, S)
**Today:** with several partners, each warp goes to the next one in order.
**Wanted:** a short menu, or the D-pad while holding L3, to pick a partner.

---

## 6. Story, shrines and cutscenes

### 6.1 Cutscene watch mode (P2, L, ?)
**Design:** when a story cutscene starts for one player, the other sees it too and is kept safe.
**Today:** nothing happens. The partner's puppet freezes or stands still while they watch.
**How:**
- Detect when your own game starts a cutscene (an event or demo starts).
- Tell your partners, so they get a banner and become invulnerable.
- Actually playing the same cutscene on the partner's side is much harder, because cutscenes depend on story progress.

### 6.2 Shrines and Divine Beasts together (P2, M)
**Today:** both players can be in the same shrine (same area id). Finishing it gives both the Spirit Orb through shared progress.
**Not handled:**
- puzzle objects inside the shrine (switches, balls, platforms) are separate in each world (see 2.3);
- a prompt to follow your partner in when they enter (design).

### 6.3 "Witnessed" story beats (P3, M)
**Design:** a guest who watched the host's story moments gets Memories and Compendium photos on their own save.
**How:** add those flags to the shared list.
- Memory flags: `Photo_*` / `MemoryN` names in `gamedata-flags.json`.
- Compendium entries: `IsGet_*` / picture book.

### 6.4 Check the shared-flag list for "flag bleed" (P1, S)
**Background:** Yui's mod broke quests by sharing the wrong world flags.
**Today:** the list is shrines, towers, Divine Beasts and Koroks (`progress.cpp`).
**Check:**
- that no shared flag changes a quest's state for a partner who hasn't done the quest;
- for example, a Divine Beast flag set early might skip parts of the other player's story.

---

## 7. Trading and items

### 7.1 Handing over cooked dishes (P2, M)
**Today:** dishes can't be held or dropped, so they can't be traded. Eating next to a partner heals them instead.
**How:**
- Pick a dish in the inventory and give it with a co-op action, for example holding L3 in the menu.
- Send its name and its cooking data (hearts, effect, duration, which live in the pouch item's data).
- Add it to the partner's inventory with `PauseMenuDataMgr::itemGet` (already found, see `offsets.hpp`) plus the cooking data.

### 7.2 Sharing food effects (P3, M)
**Today:** a shared meal only shares healing, not attack up, cold resistance, extra stamina and so on.
**How:** find where a dish's effect is applied to Link (the "potion timers" the practice mod lists for 1.6.0: speed, attack, defense, the resistances, stealth) and apply the same effect on the partner.

### 7.3 Weapons keep their bonuses (P2, S–M)
**Today:** a traded weapon arrives as the plain version.
**How:** send its bonus (attack up, durability up, etc.) with the gift. The modifier struct is the last argument of `itemGet`, or the dropped weapon actor's own data.

### 7.4 Arrows, Rupees, Korok seeds (P3, S each)
- **Arrows:** can't be dropped.
- **Rupees:** a "give Rupees" action.
- **Korok seeds:** these are already shared through progress.

### 7.5 Trading by menu (P3, M)
Picking a partner and an item from a menu, instead of dropping things near them, would be friendlier for players who don't know the Hold trick.

---

## 8. Horses

### 8.1 Your own horse on the puppet (P2, M)
**Today:** a riding partner always sits on the same brown `CoopHorse`.
**How:**
- Send which horse they ride. Look at the `GameRomHorse*` actor and its coat, mane, saddle and bridle.
- Build one physics-free horse actor per coat in `build_romfs_mod.py`, the same way gear actors are built.
- Attach the saddle, bridle and mane like armor.
- Special horses (Epona, the Royal White Stallion, the Lord of the Mountain) are separate models.

### 8.2 Riding together (P3, L)
Both on one horse, or each on their own horse side by side. The second already works with two players each on their own horse. The first would need a second seat.

### 8.3 Warping while riding (P3, S–M)
**Today:** blocked with a message.
**Wanted:** warp horse and rider together. The horse's rigid body can be moved like an enemy's.

---

## 9. Saves

### 9.1 Your real save, first run (P1, S)
**Today:** separate co-op saves haven't been run on your own profile.
**Check, before relying on it:**
- the first copy;
- that the normal slots stay unchanged;
- that the game loads the co-op copy.

How: back up `~/Library/Application Support/Ryujinx/bis/user/save/` first.

### 9.2 Choosing co-op or normal saves in game (P3, M)
**Today:** switching means editing `separate_save` in `config.ini` and restarting.
**Wanted:** a choice at the title screen.

### 9.3 More than one backup (P3, S)
**Today:** one rolling backup per launch (`coop_backup`), and a bad session followed by a relaunch overwrites the only good backup.
**Better:** keep the last 3, or back up when a session ends.

### 9.4 Save size on a real Switch (P3, S)
**Ryujinx:** has no limit.
**A real Switch:** a game's save data has a fixed size, so the co-op copy plus a backup triples it and might not fit. This only matters if the mod is ever run on hardware.

### 9.5 Starting co-op fresh (P3, S)
An option for the co-op copy to start from a new game instead of copying your save.

---

## 10. Joining, settings and the session

### 10.1 In-game session menu (P2, L, ?)
**Design:** "Open Session" / "Join Session" on the Sheikah Slate.
**Today:**
- you edit `config.ini` (server, port, password, name);
- the mod connects as soon as the game starts.

Adding menu entries needs the UI layout system found in 1.9.0.

### 10.2 A settings screen or file for players (P3, S)
`config.ini` has many `test_*` debug keys next to the real settings. Split them up, and document the real ones in the README for players.

### 10.3 Session rules the host chooses (P3, M)
**Wanted:** let the host choose:
- whether progress sharing is on;
- whether rewards are shared;
- the down time;
- friendly fire;
- enemy scaling.

Then send the choices to everyone when they join.

---

## 11. Networking

### 11.1 Protocol version and old clients (P2, S)
New message types (Heal, Downed, EnemyPos) were added. A partner on an older build ignores them silently, and the `Revived` message changed meaning (older builds expect an instant revive).
**How:** bump the protocol version, so the relay refuses mismatched builds with a clear message. The `Hello` message has a version field.

### 11.2 Lag (P2, M)
**Today:**
- enemy steering snaps when an enemy is more than 4 m off;
- puppets smooth their movement, but over real internet lag both may jerk.

**How:** test with artificial lag (macOS Network Link Conditioner), then add prediction from the sent velocity.

### 11.3 Host leaving mid-session (P2, S–M)
**Today:** the host role is passed on, and time and weather follow the new host.
**Not checked:** enemy ownership and a downed player's state when the host drops.

### 11.4 Security (P3, M)
**Today:** the relay only checks a room password, and nothing is encrypted. Fine between friends on Tailscale. Not fine for a public server.

---

## 12. Performance and stability

### 12.1 Fewer passes over the actor list (P2, S)
`combat`, `enemies`, `items` and `weapons` each walk the full actor list (300–500 actors) on their own timer. Doing one shared pass per tick would cut that work.

### 12.2 Rigid-body scans (P3, S)
**Today:** finding an enemy's body scans memory (about 20 ms for a full scan), at most every 0.5 s. That's fine at a camp, but could stutter in dense places (Hyrule Castle).
**How:** limit how many enemies are searched per pass, and scan only the regions where bodies were found before.

### 12.3 Long sessions (P2, S)
Play for an hour or two with logging on and watch for:
- growing memory;
- stale actor pointers after many area changes;
- stuck puppets;
- the network thread falling behind.

### 12.4 Ryujinx's Hypervisor mode (P3, S)
All testing used JIT mode. Ryujinx on Apple silicon can also run in Hypervisor mode, where the mod's hooks might behave differently. Try it once.

### 12.5 Debug code in normal builds (P3, S)
Compile these out of normal builds:
- `research.cpp`;
- the `test_*` helpers;
- the autostart A-presser;
- the busier logs.

---

## 13. Known bugs and rough edges

| Problem | Where | Priority |
|---|---|---|
| A downed player is held in place even mid-air or in water. | `revive.cpp` | P2 |
| Enemy copies only move sideways. Height comes from their own physics, so a jumping or flying enemy can be off in height. | `enemies.cpp` | P2 |
| The enemy body search needs the body within 0.25 m of the actor's position. Enemies with an offset body (big ones) won't be found and won't sync. | `enemies.cpp` | P2 |
| The warp's "area loaded" check is "Link moved". If Link is placed exactly on the ground and nothing moves him, the last hop waits for its 30 s timeout before saying "Warped". | `warp.cpp` | P3 |
| Holding L3 is taken by the mod for warping. A tap still crouches, but anyone who holds L3 to crouch-walk will warp by accident (only when a partner is around). | `autostart.cpp`, `warp.cpp` | P3 |
| Banners share one slot. If several events happen together, they queue 3.5 s apart, and the game's own info messages in that slot can be replaced. | `notify.cpp` | P3 |
| Gift messages only merge identical gifts in a row ("Apple x5"). Mixed stacks give one banner each. | `notify.cpp` | P3 |
| `option.sav` grew from 336 to 920 bytes in the co-op copy the first time the game saved. It looks like the game rewrites its options in a newer format. Harmless, but not understood. | Saves | P3 |
| The first-run copy happens while the game is starting. With a very large album it could add a few seconds to startup. | `saves.cpp` | P3 |
| The puppet always uses the brown horse (see 8.1). | `ghost.cpp` | P3 |
| Two Ryujinx-Nextendo windows on one Mac can hang on BotW's loading screen. The test script works around it by pausing the other copy, but players running two copies by hand won't know that. | Ryujinx quirk | P3 |

---

## 14. Shipping it to other people

### 14.1 A release package (P1 before sharing, M)
**The problem:** the mod has two parts, and the second part is built from the player's own game files. Nintendo's files can't be shared.
- **The code** (`subsdk9`, `main.npdm`): can be shared.
- **The RomFS part** (puppet actors, gear actors, `ActorInfo`, resource size table): built from the player's own game files.

**Needed:** a one-step builder for players. It takes their own base game and 1.9.0 update, runs the extraction and `build_romfs_mod.py`, and installs everything. Today that's several commands in the README.

### 14.2 The relay for players (P2, S)
- **Today:** players build the Go relay themselves.
- **Better:** ship prebuilt relays for macOS, Windows and Linux, and explain Tailscale/ZeroTier setup simply.

### 14.3 Other emulators and versions (P3, L)
- **Emulators:** only Ryujinx-Nextendo 1.10.4 on macOS has been tested. Other Ryujinx forks and yuzu-family emulators might work, since they load the same `exefs`/`romfs` mods.
- **Game version:** only BotW 1.9.0. Every address is 1.9.0-specific (see `offsets.hpp`), and the mod refuses to run on other versions.

### 14.4 Player documentation (P2, S)
**Today:** the README is written for developers.
**Needed:** a short player guide covering:
- install;
- the controls (hold L3 to warp; Hold an item and put it down near your partner to trade; eat near your partner to share a meal);
- what's shared and what isn't;
- how to get back to your normal save.

---

## 15. Suggested order

1. **Check what's built with two real games** (section 1). Start with the downed state, shared meals and enemy sync, and include separate saves on your own profile (9.1).
2. **Protocol version bump** (11.1) and the **flag bleed review** (6.4): both small, and both prevent confusing failures.
3. **Enemy sync for big and flying enemies**, and height (13), so all fights work, not only Bokoblin camps.
4. **A real down look and respawn next to your partner** (3.1, 3.3), which finishes the death loop.
5. **Your own horse** (8.1) and **weapon bonuses in trades** (7.3): visible, contained work.
6. **Handing over cooked dishes and food effects** (7.1, 7.2).
7. **Travel together** and **warp across areas** (5.1, 5.2): need reverse-engineering first.
8. **Enemy animation sync** (2.1) and **rune objects** (2.3): the biggest remaining combat work.
9. **Release package and player guide** (14) before giving it to anyone else.
10. The rest: name tags, the always-on hearts line, cutscene watch mode, menus and settings.
