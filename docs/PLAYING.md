# Playing BotW co-op

A guide for players. For how the mod is built and how it works inside, see the [README](../README.md).

Two to four people each play their own copy of Breath of the Wild in Ryujinx, in their own world, and see and help each other. Nobody is ever kicked: you can join, leave, die, fast-travel and enter shrines whenever you like.

## What you need

- A computer with [Ryujinx](https://ryujinx.app) (tested on Ryujinx-Nextendo 1.10.4 on a Mac), or a modded Switch (see [SWITCH.md](SWITCH.md); not tried on real hardware yet). Players on either can play together.
- **Your own** Breath of the Wild, dumped from your own Switch, with the **1.9.0** update. The mod refuses to run on any other version.
- The same version of this mod as your friends: everyone uses the same release package.
- Python 3.11 or newer, for the setup script.
- A way to reach the player who runs the server (see [Playing over the internet](#playing-over-the-internet)).

## Set up (once)

1. Build and install the mod from your own game files. In the mod's folder:

   ```bash
   ./setup.sh "<your base game .nsp or .xci>" "<your 1.9.0 update .nsp>"
   ```

   On Windows it's `setup.bat` with the same two files. (From the source repo, the script is `tools/setup.sh`.) It tells you what's missing if anything is, and takes a few minutes the first time. Your game files are only read.

2. Open the settings file it names at the end (`config.ini`) and set:

   | Setting | What to put |
   |---|---|
   | `name` | The name your friends will see. |
   | `server` | The address of the computer running the server, as numbers (`192.168.1.20`, not a name). The server prints the addresses to use when it starts. If the server runs on the same computer as your game, use `127.0.0.1`. |
   | `port` | `7777` unless the host says otherwise. |
   | `password` | Any word. Everyone who uses the same word plays together. |

3. In Ryujinx, turn on **Options → Settings → Network → Guest Internet Access**.

## Start a session

1. **One player** runs the server (the "relay") and leaves it running. In a release package it's `relay/relay` (`relay\relay.exe` on Windows); built from source it's:

   ```bash
   .tools/relay -listen :7777
   ```

   It prints the addresses the others put in `server`. If the computer asks whether the relay may accept incoming connections, allow it.

2. **Everyone** starts BotW and loads a save. A banner says "Co-op: connected. Waiting for a partner", or who's there ("Co-op: Zelda is here").

The first player to connect is the **host**: everyone follows their time of day, weather and Blood Moon. If the host leaves, someone else becomes host and play carries on.

Banners also tell you what's wrong: "Co-op: can't reach the server at ..." (wrong address, server not running, or a firewall in the way), "Co-op: no network connection", or "Co-op is off: your mod and the server are different versions" (someone needs to update).

## Playing over the internet

The relay is an ordinary program on one player's computer, listening on TCP port 7777. Players in the same home reach it at that computer's local address (`192.168.x.x`). Players elsewhere need one of these:

- **A private network app (easiest):** Hamachi, Tailscale, ZeroTier or Radmin VPN. The host and the friend both install it and join the same network; the friend then uses the host computer's address in that network (Hamachi: `25.x.x.x`, Tailscale: `100.x.x.x`) as `server`. Nothing to set up on the router.
- **Port forwarding:** on the host's router, forward TCP port 7777 to the host computer. Friends use the host's public IP address as `server`. Anyone who knows the address and the password can join, so pick a real password.

A Switch can't run those apps, but it doesn't need to: it connects to the relay over your home network, while your friend comes in over the private network or the forwarded port. Put the relay on a computer in the same home as the Switch.

The game data sent is small (a few kilobytes a second per player). Lag shows as partners and enemies moving a little late; it was only ever tested on one computer, so real internet lag is new ground.

## Your saves are safe

With the mod installed, the game plays from a **separate copy** of your saves.

- The first time, your saves are copied (they're only read, never changed). Co-op starts where you were.
- Everything you save while the mod is installed, solo or with friends, goes to the co-op copy.
- Each time you start the game, the co-op copy is backed up. The last three backups are kept.
- To play your normal saves again, remove the mod from Ryujinx's mods folder, or set `separate_save = false` in `config.ini`. Your normal saves are exactly as you left them.
- `backups` in `config.ini` sets how many backups are kept (0 to 3; a Switch has room for 1).

## Controls the mod adds

| Do this | And this happens |
|---|---|
| **Hold the left stick click for 1.5 seconds** | You warp to your partner and land just behind them. A quick tap still crouches. With several partners, each warp goes to the next one. |
| **Drop or throw a weapon, bow or shield next to your partner** | It leaves your world and lands at their feet. One with a bonus (Attack Up and so on) goes straight into their inventory, bonus and durability included; if their inventory is full it lands at their feet without the bonus. |
| **Inventory → pick a material or food → Hold → close the menu → put it down (A) next to your partner** | They get it. A held stack goes over one item at a time. |
| **Eat while standing next to your partner** | They're healed by the same amount. |
| **Stand next to a downed partner for a moment** | They get back up with 3 hearts. |

Warping only works when you're both outside, or both in the same shrine. Far-away partners are reached in hops through the sky, about a second per kilometre; you lose no hearts on the way. You can't warp while riding.

## What you share

- **Seeing each other.** Your partner is a second Link in your world: their armor, their weapons, what they're doing (running, climbing, swimming, gliding, fighting, riding), and a marker on your map.
- **Shrines, towers and Koroks.** When one of you clears a shrine, you all get the Spirit Orb. A tower one of you climbs reveals the map for everyone. A Korok found by one gives everyone the seed. Someone who joins later is caught up on everything already done.
- **Divine Beasts**, but only together: the Champion's ability goes to the players who are inside the beast when it's freed. (If you weren't there, you get a banner, and your own beast is still waiting for you.)
- **Enemies.** Enemies that are part of the world stand in the same place for everyone, take damage from everyone, and die for everyone.
- **Map stamps.** A stamp you put on your map shows on everyone's map: mark where to meet.
- **Time, weather and the Blood Moon**, from the host.
- **Meals**, when you eat next to each other.

Banners at the top of the screen tell you what's going on: who joined or left, what you were given, when your partner is low on hearts, when they travel or enter a shrine.

## What you don't share

- **Your inventory, Rupees, hearts and stamina** are your own.
- **Chests and drops**: everyone can open every chest in their own world.
- **Quests and the story** are your own. Cutscenes aren't shared.
- **Shrine puzzles**: you can be in the same shrine and see each other, but each of you moves your own switches and blocks.

## Going down

If a hit would kill you and you have no fairy and no Mipha's Grace, you don't get a game over while a partner is in the same area. You **go down** instead:

- You're stuck where you fell on a quarter of a heart, and your partner sees you collapse and gets a banner with how far away you are.
- They have **30 seconds** to reach you (warping counts). Standing next to you gets you up with 3 hearts.
- If nobody comes, you wake up beside your nearest partner with 1 heart.
- This can happen once every 2 minutes. A second lethal hit inside that time is a normal game over.

## New players

A player who's still on the Great Plateau (no paraglider yet) doesn't receive anyone's shrine or tower progress. That's on purpose: those first shrines hand out the runes, and skipping them would break the start of the game. Everything arrives the moment they get the paraglider.

## Things that aren't right yet

- Enemies stand in the same place for everyone, but their attacks aren't matched: your partner's Bokoblin may swing at a different moment than yours.
- Enemies keep hitting a downed player (it can't finish them).
- Bombs, Stasis, Magnesis and Cryonis only happen in your own world.
- Your partner's horse always looks the same brown.
- A traded weapon without a bonus arrives at full durability, however worn it was.
- Cooked dishes can't be handed over (eat next to your partner instead), and food effects other than healing aren't shared.
- Big enemies such as a Stone Talus may not move the same way for everyone.

The full list is in [REMAINING.md](REMAINING.md).

## If something goes wrong

| Problem | Try |
|---|---|
| No "Co-op:" banner at all after loading a save | The mod isn't loaded: check it's installed (run setup again), that the game is 1.9.0, and in Ryujinx that mods are enabled for the game. |
| "Co-op: can't reach the server at ..." | Check `server` and `port` in `config.ini` against what the relay printed, that the relay is running, that the host allowed it through the firewall, and (Ryujinx) that Guest Internet Access is on. The relay's window shows who has joined. |
| Connected, but your friend isn't there | Different `password`. Everyone with the same word plays together. |
| "Co-op is off: … different versions" | Everyone, and the server, needs the same version of the mod. |
| The game says it isn't 1.9.0 (in Ryujinx's log) | Install the 1.9.0 update in Ryujinx. |
| Two copies of the game on one Mac hang on the loading screen | A Ryujinx quirk: pause the first copy (in Ryujinx: Actions → Pause Emulation) while the second one loads. |
| You want your co-op saves from an earlier day | They're in BotW's save folder under `coop_backups/1`, `2` and `3`. Right-click the game in Ryujinx → Open User Save Directory. Copy one over the `coop` folder while the game is closed. |
