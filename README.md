# botw-coop

Seamless co-op for *The Legend of Zelda: Breath of the Wild* on Ryujinx (Switch v1.9.0). See [DESIGN.md](DESIGN.md) for what it will play like and how it works.

**Status: milestone M0.** The mod loads into the game, checks it's v1.9.0, connects to the relay server, and says hello. You can't see the other player yet; that's M1.

## Layout

| Path | What |
|---|---|
| `server/` | Relay server (Go). Rooms by password, forwards player state, hands over the host role if the host leaves. |
| `client/` | In-game mod (C++, built on [exlaunch](https://github.com/shadowninja108/exlaunch)). Builds to `subsdk9`. |
| `client/source/program/` | Our code: `main.cpp` (startup + version check), `net/` (network thread and protocol). |
| `tools/` | Helpers: deploy to Ryujinx, pull the executable out of an update NSP, decompress an NSO. |

## Requirements

- BotW v1.9.0, dumped from your own Switch, installed in Ryujinx.
- Ryujinx with **Guest Internet Access** turned on (Options → Settings → Network).
- devkitPro with devkitA64 (`/opt/devkitpro`), Go 1.23+, Python 3.

## Try M0

1. Start the relay:
   ```bash
   cd server && go run . -listen :7777
   ```
2. Build and install the mod:
   ```bash
   DEVKITPRO=/opt/devkitpro make -C client
   ./tools/deploy-ryujinx.sh
   ```
   To connect to another machine or change your name:
   ```bash
   DEVKITPRO=/opt/devkitpro make -C client COOP_FLAGS='-DCOOP_SERVER_HOST=\"192.168.1.20\" -DCOOP_PLAYER_NAME=\"Sidon\"'
   ```
3. Launch BotW in Ryujinx. The server should print:
   ```
   Link joined as player 1 (host), 1 in session
   [Link] hello from Hyrule: botw-coop M0 is running in v1.9.0
   ```
   The mod's own log lines (starting `[coop]`) appear in Ryujinx's log when guest logging is on.

## Server tests

```bash
cd server && go test -race ./...
```

## Reverse-engineering helpers

```bash
python3 -m venv .venv && .venv/bin/pip install nsz lz4
.venv/bin/python -I tools/extract_exefs.py <update.nsp> .gamefiles/exefs-1.9.0
.venv/bin/python -I tools/nso.py .gamefiles/exefs-1.9.0/main .gamefiles/main-1.9.0.bin
```

These need your console's keys in `~/.switch/prod.keys`. Game files stay in `.gamefiles/`, which is git-ignored.
