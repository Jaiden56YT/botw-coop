# BotW Seamless Co-op: Design

A seamless co-op mod for *The Legend of Zelda: Breath of the Wild* (Switch v1.9.0, the current update), running on Ryujinx. Two players (later up to four) share one Hyrule. Neither of them is a tourist in the other's game.

This document covers what the mod should feel like, how it works under the hood, and the order we build it in.

---

## 1. What we learned from prior art

| Project | What it got right | What went wrong |
|---|---|---|
| **Dark Souls / Elden Ring Seamless Co-op** (Yui) | Never kicks anyone: sessions survive deaths, boss kills and warps. One host world. Password lobbies. Group warp votes. Spectate until the party wipes. Per-player enemy scaling. Instanced loot. Separate save files, so it's safe to try. | **Flag bleed:** world-state flags leaking between worlds broke quests (burnt-and-unburnt Capital, missing areas). Spectators get stuck when a friend disconnects. Players must keep exact versions in lockstep. |
| **BotW Multiplayer / Milk Bar** (Cemu, 2023, archived 2025) | Puppet Links with animation, health and gear. Shared time and weather. Toggles for which progress syncs. Fun extra modes. | Only enemy **health** synced, not AI or position, so combat felt hollow. Read and wrote raw memory from an outside DLL. TCP messages got corrupted ("ghost Link"). Windows only, pinned to one Cemu version. |
| **Super Mario Odyssey Online** | Code injected into the game itself. Puppets are real game actors built from Mario's own model code. Simple relay server. Works on Ryujinx with Guest Internet Access turned on. | Requires one exact game version, so players have to downgrade. |

**Our rules, drawn from the above:**

1. **Never kick.** Nothing in normal play ends the session: deaths, shrines, Divine Beasts, cutscenes, fast travel, Blood Moons.
2. **One owner for every piece of state.** The host owns the world. Each player owns their own Link. Each enemy and physics object has exactly one owner at a time. No "both sides simulate it and hope".
3. **Flags go through a whitelist.** Game flags only cross between players if they're on an explicit list. Guest flags are never written into the host's world. Saves are backed up automatically.
4. **Both players progress.** A guest keeps real rewards (orbs, seeds, map, champion abilities) from things they did together.
5. **Every network state has a timeout.** A disconnect must never leave someone stuck.
6. **Packets have a length prefix and a version handshake.** Mismatched builds are refused with a clear message.

---

## 2. What it plays like

### Joining
- Both players run Ryujinx with **Guest Internet Access** turned on and the mod installed. The host runs the relay server, or a friend hosts it.
- Session password and server address go in `botw-coop.toml` next to the mod.
- **Host:** loads their co-op save and opens the session from a new Sheikah Slate menu entry, "Open Session".
- **Guest:** loads *their own* co-op save, picks "Join Session", and arrives standing next to the host.
- Joining and leaving work at any time. A guest who drops out leaves a fading ghost for 10 seconds, then disappears. The host keeps playing.

### Seeing each other
- A full **puppet Link** with the partner's armor, weapon, shield, bow, paraglider, and real animations: climbing, swimming, shield surfing, horseback.
- Name tag and health hearts above their head (can be turned off).
- Partner icon on the **map and minimap**. **Map stamps and pins are shared**, so you can mark "meet here".
- Beacons set on the Sheikah Slate are shared.

### Shared world (owned by the host)
- **Time of day and weather** follow the host.
- **Blood Moon:** when it hits the host, it hits everyone. Both players see the cutscene, and enemies respawn in both worlds.
- **Fast travel** is a vote. Anyone can propose a destination, and the other player gets a prompt to "Travel together" or "Stay". Either player can always warp to their partner from the map.
- **Shrine entry:** when one player starts entering a shrine, the other gets a prompt to follow. Both play the shrine together. When it's finished, **both get the Spirit Orb**.
- **Divine Beasts:** both enter together. Clearing one gives **both players** that Champion's ability.
- **Towers:** activating a tower reveals the region's map for **both**.
- **Korok seeds:** whoever finds a Korok, both get the seed. Each Korok can only be found once.
- **Chests and enemy loot are per player.** Everyone can open a chest once. Drops appear separately in each player's world, so nobody takes the other's loot.

### Combat that matters for both
- **One owner per enemy.** Usually the closest player when the fight begins. Ownership hands over cleanly when the owner moves far away. Everyone else sees the same enemy moving, attacking and reacting, because it's a puppet driven by the owner.
- **Hits from either player count.** Damage, element effects (fire, shock, freeze), knockback, and stuns all reach the owning player's game.
- **Scaling per player** in the config: enemy health, damage, and stun/stagger time.
- **Rune combos work across players.** One player freezes an enemy with Stasis while the other winds up a hit. One throws a bomb while the other launches a boulder with Magnesis. The physics object is owned by whoever touched it last, so it behaves the same for both.
- **Friendly fire:** off by default. A "Rivals" option turns it on.

### Death and revival
In normal BotW, death reloads the save. That would break the session, so in co-op:
1. Hitting 0 hearts puts you into a **downed** state. You can crawl but not attack.
2. Your partner holds **A** next to you for 3 seconds to **revive** you with a few hearts.
3. If nobody revives you within 30 seconds, you **respawn next to your partner** with a short "weakened" penalty (less stamina for 60 seconds).
4. If **both** players are downed at once, the whole party respawns at the host's last safe point. Nothing is reloaded from disk.
5. Fairies still work the way you'd expect, as an automatic self-revive.

### Story and cutscenes
- When a main-story event starts for one player, the other's screen fades into **watch mode**: they see the cutscene too, and their Link is safely frozen.
- Quest progress belongs to the host world. Guests get a "**Witnessed**" mark on their own save for story beats they saw. It unlocks their Memories and the Hyrule Compendium entries they photographed, but it doesn't skip their own story.

### Trading
- Drop a weapon, bow, shield or material near your partner and it appears in their world to pick up.
- Materials go through BotW's **Hold**: hold them from the menu, then put them down or throw them next to your partner.
- Cooked meals can't be held, so they aren't shared yet.

### Saves
- Co-op uses **separate save files**, kept in their own folder. Your normal save is never touched.
- The mod backs up the co-op save automatically every time the session opens.

---

## 3. Architecture

```
┌──────────── Ryujinx (Player A) ─────────────┐        ┌──────── Relay server ────────┐
│  BotW 1.9.0                                  │        │  rooms, password, versions   │
│   └─ subsdk9 (our mod, C++, exlaunch)        │  TCP   │  forwards packets            │
│       ├─ hooks/   (player, actors, flags)    │◀──────▶│  decides who owns what       │
│       ├─ puppet/  (remote Links, enemies)    │        │  host-world state snapshot   │
│       ├─ sync/    (world, combat, progress)  │        └──────────────────────────────┘
│       └─ net/     (socket thread, queues)    │                    ▲
└──────────────────────────────────────────────┘                    │
                                                         Ryujinx (Player B) … same mod
```

### In-game mod (`client/`)
- **Language and framework:** C++20, built with devkitA64 and **exlaunch**. Output is `exefs/subsdk9` + `main.npdm`, dropped into Ryujinx's mods folder (`mods/contents/01007EF00011E000/botw-coop/exefs/`). Starting point: upstream exlaunch. The game's build ID for 1.9.0 is `cd57b23fa4bbad65803d9788c01821ee`.
- **Names and structures** come from the **zeldaret/botw** decomp headers. Game flags, time and weather, the event system and quests are almost fully understood there. The actor system and AI are about half done, so they need our own reverse-engineering.
- **Threads:** a socket thread does all networking. It talks to the game thread only through queues that never block. The game thread never waits on the network.
- **Hooks (first set):**
  - Player update each frame: send our Link's position, animation and gear.
  - Game-thread frame tick: drain the incoming queue and apply it.
  - Flag writes: catch whitelisted flag changes to send to the partner.
  - Damage handling on actors: send hits on enemies we don't own to their owner.
  - Death: replace "game over" with downed / respawn.
  - Stage change: shrine and Divine Beast entry, fast travel.

### Relay server (`server/`)
- **Go**, a single small program that runs on macOS, Windows and Linux. No port forwarding needed if both players use Tailscale or ZeroTier.
- Holds rooms (password + host), checks the version handshake, forwards packets, and **decides who owns each enemy and object** (so both clients never disagree).
- Keeps the latest snapshot of host-world state so late joiners catch up instantly.

### Protocol
- Every message: `u32 length | u16 type | u16 flags | payload`. Little-endian.
- **Handshake** first: mod version, game build hash, player name, password. A mismatch gets a clear refusal.
- Rates: own-Link state 30 times per second, enemy state 15–20 times per second for enemies near any player, world state when it changes plus once a second.
- Only things near at least one player get synced, so distant parts of Hyrule cost nothing.
- TCP for everything at first. Link and enemy movement can move to UDP later if lag shows up.

### Who owns what

| State | Owner |
|---|---|
| Each player's own Link, inventory, hearts, stamina | That player |
| Time, weather, Blood Moon, quests, world flags | Host |
| Each enemy / NPC in a fight | One player at a time, assigned by the server |
| Physics objects moved by runes | Whoever touched it last |
| Chests, drops | Each player separately |
| Shrine / Korok / tower / Divine Beast completions | Shared: credited to everyone present |

---

## 4. Build order

Each milestone is playable before the next one starts.

| # | Milestone | Done when |
|---|---|---|
| **M0** | **Hello Hyrule** | The mod loads in Ryujinx, connects to the relay server, and logs Link's position from inside the game. |
| **M1** | **Ghosts** | Two Ryujinx windows; each shows a marker or simple model for the other Link, moving smoothly. Partner on the map. |
| **M2** | **Real puppets** | The partner is a full Link with gear and animations. Shared time and weather. Warp to partner. Shared map stamps. |
| **M3** | **Fighting together** | Enemy ownership, damage from both players, scaling, downed and revive, synced bombs and rune objects. |
| **M4** | **Progress** | Shrines and Divine Beasts together, shared Korok seeds and towers, flag whitelist, separate co-op saves with backups. |
| **M5** | **Polish** | Cutscene watch mode, Blood Moon, fast-travel voting, trading, horses, the Rivals mode, 3–4 players. |

**Progress (2026-11-09):**
- **M0** is done.
- **M1** is done, including the partner's marker on the map (a real map stamp, moved every second).
- **M2 so far:**
  - Other players are an animated Link puppet showing their real movement state (standing, walking, running, sprinting, climbing, swimming, gliding, jumping, falling, riding, shield surfing), with shared time of day.
  - Not yet: gear, attack and item animations, warp to partner.
- **M4 so far:**
  - Shrine, tower, Divine Beast and Korok flags are shared both ways, with catch-up for late joiners. Checked with a 100% save: 1,327 flags sent to a late joiner.
  - Live partner progress also gives rewards: Spirit Orbs, Korok seeds and Champion abilities. A Spirit Orb was seen arriving in Key Items.
  - Not yet: map reveal from towers, separate co-op saves.
- **Areas:** players send which stage they're in. Partners elsewhere are hidden; on the map they stay at the shrine they entered.
- **M2:** the puppet wears the partner's armor (one gear actor per armor model, animated with the body) and carries their weapons on the right bones (the game's own bone binding), and shows their attacks, bow, throws and guard. The host's weather and Blood Moon are shared. A partner's tower reveals your map. Not yet: warp to partner (writing Link's position doesn't move him; fast travel is UI-driven).
- **M3 so far:** placed enemies (same map HashId in every world) share their life. Hits from either player count, and a kill in one world is a kill in all. **Co-op revive:** with no fairy or Mipha's Grace, a partner nearby gets you back up with 3 hearts instead of a game over (once every 2 minutes). Not yet: enemy movement sync, a crawl-and-revive downed state, friendly fire.
- **M5 so far:** trading weapons and materials (drop, put down or throw near a partner), a horse under a riding partner, three or more players at once.
- **Progress catch-up goes both ways:** a joiner gets everything done so far, and everyone else gets the joiner's progress.
- **Saves:** for now co-op uses a separate Ryujinx user profile, which has its own saves.

---

## 5. Biggest unknowns (to investigate early)

1. **How to create a puppet Link.** The game only expects one player. Options: create a second actor from Link's own model with its brain turned off, or add a custom puppet actor through a romfs mod. This needs a research spike during M1.
2. **Enemy puppets.** Turning off an enemy's AI on non-owners while still showing its animations. The AI code is only partly decompiled.
3. **Cutscenes and stage changes** while a second player exists.
4. **Saving to a separate place** instead of the normal save slots.
5. **Ryujinx on Mac:** whether our hooks behave the same in the Hypervisor CPU mode as in JIT mode.
6. **Game version:** we target **v1.9.0**, the update every Switch downloads today, so nobody has to downgrade. The decomp is built against v1.5.0, so its names describe the right code but its addresses don't match. Every function we hook has to be found again in 1.9.0 (by matching code patterns, strings and call structure) and recorded in `client/src/offsets/1.9.0.hpp`. The mod checks the game's build ID at startup and refuses to run on any other version.

---

## 6. Requirements for players and developers

- Ryujinx (community build, e.g. Ryubing), with **Guest Internet Access** turned on and a non-default user profile.
- BotW **v1.9.0** for Switch, **dumped from a Switch you own**, with that console's keys.
- For building: devkitPro with devkitA64, CMake, and Go for the server.
- For reverse-engineering: `tools/extract_exefs.py` pulls the executable out of an update NSP, and `tools/nso.py` decompresses it into a flat image for a disassembler.
