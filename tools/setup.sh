#!/bin/bash
# One-step setup for macOS and Linux: builds the co-op mod from YOUR OWN copy
# of the game and installs it into Ryujinx.
#
#   ./setup.sh "<base game .nsp or .xci>" "<1.9.0 update .nsp>"
#
# For a modded Switch, add:  --switch <folder>   (see docs/SWITCH.md)
# Everything it does is in tools/coop_setup.py; this only finds Python.
#
# Settings (environment):
#   RYU_PATH=<folder>   Ryujinx's data folder, if it isn't in the usual place
#   NO_INSTALL=1        build everything but don't install into Ryujinx
#   GAMEFILES=<folder>  where the files pulled from your dump go (default: .gamefiles)
#   ROMFS_OUT=<folder>  where the built actors go (default: client/romfs)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/coop_setup.py"
[ -f "$SCRIPT" ] || SCRIPT="$HERE/tools/coop_setup.py"

PY=""
# The first `python3` can be an old one (macOS ships 3.9 with Xcode), so newer
# ones are looked for by name and in the usual install places too.
for p in python3 python3.14 python3.13 python3.12 python3.11 python3.15 python \
    /opt/homebrew/bin/python3 /usr/local/bin/python3 /Library/Frameworks/Python.framework/Versions/Current/bin/python3; do
    if command -v "$p" >/dev/null && "$p" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; then
        PY="$p"
        break
    fi
done
if [ -z "$PY" ]; then
    echo "Setup stopped: Python 3.11 or newer isn't installed. Get it from https://www.python.org/downloads/ (macOS: or brew install python; Linux: your package manager), then run this again." >&2
    exit 1
fi
exec "$PY" "$SCRIPT" "$@"
