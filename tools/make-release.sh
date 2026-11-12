#!/bin/bash
# Makes the packages to give to other players: dist/botw-coop-<system>.zip for
# Windows, macOS (Apple silicon and Intel) and Linux.
#
#   tools/make-release.sh
#
# Each package holds the mod's code already built (client/deploy), the relay
# server and the dump reader as ready-to-run programs for that system, the
# setup script, the player guides and the source. It holds NO game files: each
# player builds those from their own dump with the setup script.
#
# Needs: the mod built (make -C client), Go, and .NET (for the dump reader).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DIST="$ROOT/dist"
VERSION="$(date +%Y-%m-%d)-$(git rev-parse --short HEAD 2>/dev/null || echo dev)"
# Built from changes that aren't committed yet: say so, with the code's own fingerprint.
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    VERSION="$VERSION+$(shasum client/deploy/subsdk9 2>/dev/null | cut -c1-7)"
fi

[ -f client/deploy/subsdk9 ] && [ -f client/deploy/main.npdm ] || { echo "build the mod first: make -C client" >&2; exit 1; }
# The code's symbols, for Savage Wind Recomp (docs/RECOMP.md): made from the same build, and named after it.
NM="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-nm"
[ -x "$NM" ] && [ -f client/client.elf ] || { echo "client/client.elf (or devkitA64's nm) is missing: make -C client" >&2; exit 1; }
{ echo "# symbols of subsdk9 $(shasum client/deploy/subsdk9 | cut -d' ' -f1)"; "$NM" --defined-only client/client.elf | awk 'NF == 3'; } > client/deploy/subsdk9.mod
if [ -d /opt/homebrew/opt/dotnet/libexec ] && [ -z "${DOTNET_ROOT:-}" ]; then export DOTNET_ROOT=/opt/homebrew/opt/dotnet/libexec; fi

# system name : .NET runtime id : GOOS : GOARCH : program suffix
SYSTEMS="windows-x64:win-x64:windows:amd64:.exe macos-arm64:osx-arm64:darwin:arm64: macos-intel:osx-x64:darwin:amd64: linux-x64:linux-x64:linux:amd64:"

rm -rf "$DIST/pkg"
mkdir -p "$DIST/pkg"
for sys in $SYSTEMS; do
    IFS=: read -r name rid goos goarch exe <<<"$sys"
    out="$DIST/pkg/botw-coop-$name/botw-coop"
    echo "== $name"
    mkdir -p "$out"
    # The source, as git sees it (tracked and new files; nothing ignored, so no game files).
    git ls-files -co --exclude-standard | grep -v '^dist/' | while read -r f; do
        [ -f "$f" ] || continue
        mkdir -p "$out/$(dirname "$f")"
        cp "$f" "$out/$f"
    done
    # The mod's code, built.
    mkdir -p "$out/client/deploy"
    cp client/deploy/subsdk9 client/deploy/main.npdm client/deploy/subsdk9.mod "$out/client/deploy/"
    # The relay.
    mkdir -p "$out/relay"
    (cd server && CGO_ENABLED=0 GOOS=$goos GOARCH=$goarch go build -trimpath -ldflags "-s -w" -o "$out/relay/relay$exe" .)
    # The dump reader, with .NET inside so players don't install it.
    pub="$ROOT/.tools/publish/$rid"
    if [ ! -f "$pub/RomfsTool$exe" ] || [ tools/RomfsTool/Program.cs -nt "$pub/RomfsTool$exe" ]; then
        dotnet publish tools/RomfsTool -c Release -r "$rid" --self-contained true -p:PublishSingleFile=true \
            -p:EnableCompressionInSingleFile=true -p:DebugType=none -o "$pub" >/dev/null
    fi
    mkdir -p "$out/tools/bin"
    cp "$pub/RomfsTool$exe" "$out/tools/bin/"
    # The setup script at the top, and a short "start here".
    if [ "$goos" = windows ]; then
        cp tools/setup.bat "$out/setup.bat"
        START='setup.bat "C:\path\to\base game.nsp" "C:\path\to\1.9.0 update.nsp"'
        RELAY='relay\relay.exe'
    else
        cp tools/setup.sh "$out/setup.sh"
        chmod +x "$out/setup.sh" "$out/relay/relay" "$out/tools/bin/RomfsTool"
        START='./setup.sh "/path/to/base game.nsp" "/path/to/1.9.0 update.nsp"'
        RELAY='relay/relay'
    fi
    VERSION="$VERSION" SYSTEM="$name" START="$START" RELAY="$RELAY" python3 -c '
import os, sys
text = open(sys.argv[1]).read()
for key in ("VERSION", "SYSTEM", "START", "RELAY"):
    text = text.replace("@" + key + "@", os.environ[key])
open(sys.argv[2], "w", newline="\r\n" if "windows" in os.environ["SYSTEM"] else "\n").write(text)
' tools/release-readme.txt "$out/START HERE.txt"
    echo "$VERSION" > "$out/VERSION"
    (cd "$DIST/pkg/botw-coop-$name" && rm -f "$DIST/botw-coop-$name.zip" && zip -qr "$DIST/botw-coop-$name.zip" botw-coop)
    ls -lh "$DIST/botw-coop-$name.zip" | awk '{print $5, $9}'
done
rm -rf "$DIST/pkg"
