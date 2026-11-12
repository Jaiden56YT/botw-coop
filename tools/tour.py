"""Test helper: a long co-op session that tours the map (Savage Wind Recomp test copies).

    python3 tools/tour.py <minutes> <out dir> [seed]

Copy 1 (control port 7801) is the leader: every step it jumps to a shrine,
tower, stable, village or landmark picked at random (`swctl goto`). The other
players are expected to follow by themselves (`test_follow` in their
config.ini): recomp copy 2, and a Ryujinx test player if one is on the relay.
For each step the script waits until every follower stands by the leader,
then writes one line to <out dir>/tour.log: how long each took and what the
mod logged meanwhile (warps, hops that didn't take, falls, downs). It also
keeps a screenshot of the leader's view per step.

Reads positions from the relay's log (RELAY_LOG) and each game's log.
"""
import json
import math
import os
import random
import re
import subprocess
import sys
import time

SW = os.path.expanduser("~/SavageWindRecomp")
RELAY_LOG = os.environ.get("RELAY_LOG", "")
LEADER = os.environ.get("LEADER", "Recomp")
FOLLOWERS = os.environ.get("FOLLOWERS", "Recomp2,Claude").split(",")
LOGS = dict(pair.split("=", 1) for pair in os.environ.get("LOGS", "").split(",") if "=" in pair)
ARRIVE_M = 15.0
STEP_TIMEOUT = 150
DWELL = 20

ALL_KINDS = {"shrine": [("shrine", 0, 119)] * 5, "tower": [("tower", 1, 15)] * 2, "stable": [("stable", 1, 15)] * 2,
             "village": [("village", 1, 9)], "landmark": [("landmark", 1, 9)]}
KINDS = [k for name in os.environ.get("KINDS", "shrine,tower,stable,village,landmark").split(",") for k in ALL_KINDS[name]]
WATCH = [
    ("arrived", r"warp: arrived"),
    ("no-take", r"didn't take"),
    ("stopped", r"notify: Warp stopped"),
    ("fell", r"the landing spot; back to"),
    ("down", r"revive: you're down"),
    ("up", r"got you back up"),
    ("nobody", r"nobody reached you"),
    ("respawn", r"weapons: .*respawn|ghost: .*respawn"),
]


def ctl(port, *args, timeout=10):
    try:
        r = subprocess.run([f"{SW}/tools/venv/bin/python", f"{SW}/tools/swctl.py", "--json", "--port", str(port), *args],
                           capture_output=True, text=True, timeout=timeout, cwd=SW)
        return json.loads(r.stdout).get("data") if r.stdout.strip() else None
    except Exception:
        return None


def relay_positions():
    """Last position the relay printed for each player, with the line number."""
    out = {}
    try:
        with open(RELAY_LOG, "rb") as f:
            f.seek(max(0, os.path.getsize(RELAY_LOG) - 20000))
            tail = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return out
    for line in tail:
        m = re.search(r"(\d\d:\d\d:\d\d) \[(.+?)\] at \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", line)
        if m:
            h, mi, s = (int(v) for v in m.group(1).split(":"))
            out[m.group(2)] = (h * 3600 + mi * 60 + s, float(m.group(3)), float(m.group(4)), float(m.group(5)))
    return out


def log_size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def log_counts(path, start):
    counts = {}
    try:
        with open(path, "rb") as f:
            f.seek(start)
            text = f.read().replace(b"\0", b"").decode("utf-8", "replace")
    except OSError:
        return counts
    for key, pattern in WATCH:
        n = len(re.findall(pattern, text))
        if n:
            counts[key] = n
    return counts


def now_s():
    t = time.localtime()
    return t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec


def main():
    minutes, out = float(sys.argv[1]), sys.argv[2]
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "tour.log"), "a", buffering=1)

    def say(text):
        log.write(time.strftime("%H:%M:%S ") + text + "\n")

    end = time.time() + minutes * 60
    step = 0
    misses = {name: 0 for name in FOLLOWERS}
    say(f"tour: {minutes:.0f} min, leader {LEADER}, followers {FOLLOWERS}")
    while time.time() < end:
        step += 1
        kind, lo, hi = rng.choice(KINDS)
        number = rng.randint(lo, hi)
        sizes = {name: log_size(path) for name, path in LOGS.items()}
        started = time.time()
        started_clock = now_s()
        reply = ctl(7801, "goto", kind, str(number))
        if reply is None:
            say(f"step {step}: {kind} {number}: the leader doesn't answer")
            time.sleep(20)
            continue
        # The leader has landed once his position stops changing.
        last, still = None, 0
        while time.time() - started < 60 and still < 3:
            time.sleep(1)
            pos = ctl(7801, "pos")
            cur = (pos["x"], pos["y"], pos["z"]) if pos and "x" in pos else None
            still = still + 1 if cur and last and math.dist(cur, last) < 0.05 else 0
            last = cur
        if not last:
            say(f"step {step}: {kind} {number}: no position from the leader")
            continue
        took = {}
        while time.time() - started < STEP_TIMEOUT and len(took) < len(FOLLOWERS):
            time.sleep(2)
            pos = ctl(7801, "pos")
            if pos and "x" in pos:
                last = (pos["x"], pos["y"], pos["z"])
            seen = relay_positions()
            for name in FOLLOWERS:
                if name in took or name not in seen:
                    continue
                at, x, y, z = seen[name]
                if (at - started_clock) % 86400 < 2 or (at - started_clock) % 86400 > 3600:
                    continue  # printed before this step began
                if math.dist((x, y, z), last) > ARRIVE_M:
                    continue
                # Held at the landing spot while the area loads isn't "there" yet: wait for the warp to end.
                if name in LOGS and not log_counts(LOGS[name], sizes[name]).get("arrived"):
                    continue
                took[name] = time.time() - started
        ctl(7801, "shot", os.path.relpath(os.path.join(out, f"step{step:03d}.png"), SW))
        parts = []
        for name in FOLLOWERS:
            if name in took:
                misses[name] = 0
                parts.append(f"{name} {took[name]:.0f}s")
            else:
                misses[name] += 1
                seen = relay_positions().get(name)
                where = f"{math.dist(seen[1:], last):.0f} m away" if seen else "not on the relay"
                parts.append(f"{name} NOT HERE ({where}, miss {misses[name]})")
        notes = []
        for name, path in LOGS.items():
            counts = log_counts(path, sizes[name])
            if counts:
                notes.append(name + ": " + " ".join(f"{k}={v}" for k, v in counts.items()))
        say(f"step {step}: {kind} {number} at ({last[0]:.0f}, {last[1]:.0f}, {last[2]:.0f}): " + ", ".join(parts) +
            (" | " + "; ".join(notes) if notes else ""))
        time.sleep(DWELL)
    say("tour: done")


if __name__ == "__main__":
    main()
