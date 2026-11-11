#!/bin/bash
# Copies the built mod into Ryujinx's mods folder for BotW.
# Only subsdk9 is installed: the game's own main.npdm is kept.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RYU="${RYU_PATH:-$HOME/Library/Application Support/Ryujinx}"
DEST="$RYU/mods/contents/01007ef00011e000/botw-coop/exefs"

if [ ! -f "$ROOT/client/deploy/subsdk9" ]; then
    echo "client/deploy/subsdk9 not found; build first with: make -C client" >&2
    exit 1
fi

mkdir -p "$DEST"
cp "$ROOT/client/deploy/subsdk9" "$DEST/subsdk9"
echo "Installed to $DEST"

# Game files the mod adds or extends (built from your own dump by
# tools/build_romfs_mod.py; never committed).
ROMFS="$RYU/mods/contents/01007ef00011e000/botw-coop/romfs"
if [ -d "$ROOT/client/romfs" ]; then
    mkdir -p "$ROMFS"
    cp -R "$ROOT/client/romfs/." "$ROMFS/"
    echo "Installed RomFS files to $ROMFS"
else
    echo "No client/romfs yet: run tools/build_romfs_mod.py to build the puppet actors" >&2
fi

# Settings live in the mod's RomFS folder, which Ryujinx layers into the
# game's read-only files, so the game can read them without extra permissions.
# Edit this file to change your name, the server address or the password.
CONFIG="$RYU/mods/contents/01007ef00011e000/botw-coop/romfs/botw-coop/config.ini"
if [ ! -f "$CONFIG" ]; then
    mkdir -p "$(dirname "$CONFIG")"
    cat > "$CONFIG" <<'INI'
# botw-coop settings. How to play: docs/PLAYING.md

# The name your partners see.
name = Link

# Where the relay server is. The player who runs it uses 127.0.0.1; everyone
# else uses that player's address (same Wi-Fi, Tailscale or ZeroTier).
server = 127.0.0.1
port = 7777

# Everyone with the same password plays together.
password = hyrule

# Keep co-op in its own copy of your saves, so your normal saves are never
# changed. Set to false to play (and change) your normal saves.
separate_save = true

# How many backups of the co-op saves to keep (one is made each time the game
# starts). 0 to 3. A real Switch only has room for 1.
backups = 3

# A Divine Beast only counts for players who are inside it together. Set to
# true to give its Champion ability to partners anywhere.
beasts_anywhere = false

# The rest is for development (the full list is in the README).
ghost_actor = CoopPlayer
echo = false
INI
    echo "Wrote default settings to $CONFIG"
fi
