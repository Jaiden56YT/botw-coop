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
