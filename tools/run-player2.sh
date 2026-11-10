#!/bin/bash
# Test helper: (re)starts a test copy of Ryujinx ("player 2") on one Mac.
#
#   tools/run-player2.sh [log file]
#
# - Installs the current build into ~/botw-coop/.ryujinx-p2. Set
#   P_DIR=<folder> to use another test copy (e.g. .ryujinx-p1, for a test with
#   two test copies and none of your own setup involved).
# - Mutes the test copy (set P2_SOUND=1 to hear it).
# - Copies your BotW save from the main Ryujinx folder, so the test copy loads it.
#   Set P2_SAVE=<folder with slots 0-7, option.sav, ...> to use that save instead
#   (e.g. a 100% save for testing); your own save is never touched either way.
# - A second Ryujinx-Nextendo often hangs forever on BotW's loading screen
#   while another one is running (see docs/reverse-engineering.md), so every
#   other running Ryujinx is suspended (SIGSTOP) until this one reaches the
#   world, then resumed (SIGCONT). They're always resumed, even on failure or Ctrl-C.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P2="${P_DIR:-$ROOT/.ryujinx-p2}"
case "$P2" in /*) ;; *) P2="$ROOT/$P2" ;; esac
MAIN="$HOME/Library/Application Support/Ryujinx"
LOG="${1:-$P2/player.log}"
GAME="${BOTW_NSP:-$HOME/Downloads/games/Switch Games/The Legend of Zelda Breath of the Wild [01007EF00011E000][v0] (13.48 GB).nsp}"
RYU=/Applications/Ryujinx.app/Contents/MacOS/Ryujinx

pkill -f "root-data-dir $P2" && sleep 2
RYU_PATH="$P2" "$ROOT/tools/deploy-ryujinx.sh" >/dev/null

# A test copy: keep it silent so it doesn't play over your game.
if [ -z "${P2_SOUND:-}" ] && [ -f "$P2/Config.json" ]; then
    sed -i '' -E 's/"audio_volume": *[0-9.]+/"audio_volume": 0/' "$P2/Config.json"
fi

SAVE_ROOT="$P2/bis/user/save/0000000000000005"
rm -rf "$SAVE_ROOT"
if [ -n "${P2_SAVE:-}" ]; then
    # Ryujinx keeps a committed (0) and a working (1) copy of the save.
    mkdir -p "$SAVE_ROOT/0" "$SAVE_ROOT/1"
    cp -R "$P2_SAVE/." "$SAVE_ROOT/0/"
    cp -R "$P2_SAVE/." "$SAVE_ROOT/1/"
    cp "$MAIN/bis/user/save/0000000000000005/"ExtraData* "$SAVE_ROOT/" 2>/dev/null || true
    echo "save: $P2_SAVE"
else
    cp -R "$MAIN/bis/user/save/0000000000000005" "$P2/bis/user/save/"
fi

# Every other Ryujinx that's running (your own game or another test copy).
OTHERS=$(pgrep -f "$RYU" | while read -r pid; do
    ps -o command= -p "$pid" | grep -q -- "--root-data-dir $P2" || echo "$pid"
done | tr '\n' ' ')

resume() { for pid in $OTHERS; do kill -CONT "$pid" 2>/dev/null; done; }
trap resume EXIT INT TERM

nohup "$RYU" --root-data-dir "$P2" "$GAME" > "$LOG" 2>&1 &
echo "$(basename "$P2") starting (log: $LOG)"

for pid in $OTHERS; do
    kill -STOP "$pid" && echo "Ryujinx pid $pid suspended while this one loads"
done

# The mod logs its area when Link reaches the world; wait up to 2 min.
for i in $(seq 1 60); do
    if tr -d '\000' < "$LOG" | grep -q -E "\[coop\] area:|spawned ghost|world: synced|Fatal error|Unhandled exception"; then
        break
    fi
    sleep 2
done
resume
[ -n "$OTHERS" ] && echo "others resumed after ~$((i * 2)) s"
tr -d '\000' < "$LOG" | grep -E "\[coop\] area:|spawned ghost|couldn't spawn|Fatal error|Unhandled exception" | tail -3
