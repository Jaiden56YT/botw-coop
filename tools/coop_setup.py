#!/usr/bin/env python3
"""One-step setup: builds the co-op mod from YOUR OWN copy of the game and
installs it. Works on macOS, Windows and Linux; needs only Python 3.11+.

    python3 tools/coop_setup.py "<base game .nsp or .xci>" "<1.9.0 update .nsp>"

By default it installs into Ryujinx. For a real (modded) Switch, add
--switch <folder>: the folder gets an `atmosphere` folder to copy onto the SD
card (give the SD card itself to write it there directly).

The mod's puppet actors (Link, armor, weapons, a horse) are made from the
game's own files, which can't be shared, so each player builds them from
their own dump. Nothing in the dump is changed.

What it does:
  1. checks what it needs (your console keys, a few Python packages)
  2. pulls the handful of files it needs out of your dump into .gamefiles/
  3. builds the puppet actors into client/romfs/
  4. builds the mod's code if devkitPro and the source are here, else uses the
     prebuilt client/deploy/subsdk9
  5. builds the relay server if Go and its source are here, else uses relay/
  6. installs into Ryujinx and/or writes the Switch SD card folder
"""
import os
import sys

# Only the standard library is wanted here: keep this folder's own scripts
# from being imported in place of standard modules of the same name.
_here = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or ".") != _here]

if sys.version_info < (3, 10):
    sys.exit("Setup stopped: this needs Python 3.11 or newer (this is %d.%d). Get it from python.org."
             % sys.version_info[:2])

# A file name the console can't show mustn't stop the setup.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

import argparse
import platform
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TITLE = "01007ef00011e000"
WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""

CONFIG_TEMPLATE = """# botw-coop settings. How to play: docs/PLAYING.md

# The name your partners see.
name = Link

# Where the relay server is. The player who runs it on the same computer as
# their game uses 127.0.0.1; everyone else uses that computer's address (the
# relay prints the addresses to use when it starts).
server = {server}
port = 7777

# Everyone with the same password plays together.
password = hyrule

# Keep co-op in its own copy of your saves, so your normal saves are never
# changed. Set to false to play (and change) your normal saves.
separate_save = true

# How many backups of the co-op saves to keep (one is made each time the game
# starts). 0 to 3. A real Switch only has room for 1.
backups = {backups}

# Also write the mod's log to the SD card (sd:/botw-coop/log.txt). A Switch
# has no other way to show it; in Ryujinx the log is in Ryujinx's own console.
log_file = {log_file}

# A Divine Beast only counts for players who are inside it together. Set to
# true to give its Champion ability to partners anywhere.
beasts_anywhere = false

# The rest is for development (the full list is in the README).
ghost_actor = CoopPlayer
echo = false
"""


def say(text):
    print(f"\n== {text}", flush=True)


def fail(text):
    print(f"\nSetup stopped: {text}", file=sys.stderr)
    sys.exit(1)


def ryujinx_dirs():
    """Where Ryujinx keeps its data on this computer (most likely first)."""
    home = Path.home()
    if WINDOWS:
        base = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
        return [base / "Ryujinx"]
    if sys.platform == "darwin":
        return [home / "Library" / "Application Support" / "Ryujinx"]
    return [home / ".config" / "Ryujinx", home / ".var/app/org.ryujinx.Ryujinx/config/Ryujinx"]


def check_keys():
    keys = Path.home() / ".switch" / "prod.keys"
    if keys.is_file():
        return
    for ryu in ryujinx_dirs():
        src = ryu / "system" / "prod.keys"
        if src.is_file():
            keys.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, keys)
            print(f"Copied your console keys from Ryujinx ({src}) to {keys}")
            title = src.with_name("title.keys")
            if title.is_file():
                shutil.copyfile(title, keys.with_name("title.keys"))
            return
    fail(f"your console keys aren't at {keys}. They're needed to read your dump: "
         "put the prod.keys you dumped from your own Switch there (the same file Ryujinx asks for).")


def venv_python():
    venv = ROOT / ".venv"
    py = venv / ("Scripts/python.exe" if WINDOWS else "bin/python")
    if not py.is_file():
        print("Setting up Python tools in .venv (once)")
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    probe = [str(py), "-I", "-c", "import oead, rstb"]
    if subprocess.run(probe, capture_output=True).returncode != 0:
        print("Installing the Python packages oead and rstb")
        r = subprocess.run([str(py), "-m", "pip", "install", "--quiet", "oead", "rstb"])
        if r.returncode != 0 or subprocess.run(probe, capture_output=True).returncode != 0:
            fail("the Python packages oead and rstb didn't install. They need Python 3.11 to 3.14 "
                 f"(this is {platform.python_version()}); install one from python.org and run this again.")
    return py


def romfs_tool():
    """The dump reader: the prebuilt one if this package has it, else `dotnet run`."""
    prebuilt = ROOT / "tools" / "bin" / f"RomfsTool{EXE}"
    if prebuilt.is_file():
        if not WINDOWS:
            prebuilt.chmod(0o755)
        return [str(prebuilt)]
    env_root = Path("/opt/homebrew/opt/dotnet/libexec")
    if env_root.is_dir() and "DOTNET_ROOT" not in os.environ:
        os.environ["DOTNET_ROOT"] = str(env_root)
    if not shutil.which("dotnet") or not (ROOT / "tools" / "RomfsTool").is_dir():
        fail(".NET isn't installed (needed to read the dump). Install the .NET SDK "
             "(macOS: brew install dotnet; Windows: winget install Microsoft.DotNet.SDK.10) and run this again.")
    return ["dotnet", "run", "--project", str(ROOT / "tools" / "RomfsTool"), "--"]


def extract_files(tool, base, update, gamefiles):
    (gamefiles / "packs").mkdir(parents=True, exist_ok=True)

    def run(args, what):
        r = subprocess.run(tool + [str(base), str(update)] + args, capture_output=True, text=True)
        if r.returncode != 0:
            tail = "\n".join((r.stderr or r.stdout).strip().splitlines()[-3:])
            fail(f"couldn't read {what} from the dump. Is the first file the base game and the second the "
                 f"1.9.0 update, and are the keys from the same console?\n{tail}")

    def one(path, dest):
        if dest.is_file() and dest.stat().st_size > 0:
            return
        print(f"  {path}", flush=True)
        run(["extract", path, str(dest)], path)

    def prefix(pre, dest, what):
        if dest.is_dir() and any(dest.iterdir()):
            return
        print(f"  every {what}", flush=True)
        dest.mkdir(parents=True, exist_ok=True)
        run(["extract-prefix", pre, str(dest)], pre + "*")

    one("Actor/ActorInfo.product.sbyml", gamefiles / "ActorInfo-1.9.0.sbyml")
    one("System/Resource/ResourceSizeTable.product.srsizetable", gamefiles / "ResourceSizeTable.product.srsizetable")
    one("Actor/Pack/Npc_DungeonPriest001.sbactorpack", gamefiles / "packs" / "Npc_DungeonPriest001.sbactorpack")
    one("Actor/Pack/PauseMenuPlayer.sbactorpack", gamefiles / "packs" / "PauseMenuPlayer.sbactorpack")
    one("Actor/Pack/GameRomHorse01.sbactorpack", gamefiles / "packs" / "GameRomHorse01.sbactorpack")
    one("Pack/Bootup.pack", gamefiles / "Bootup.pack")
    prefix("/Actor/Pack/Armor_", gamefiles / "armor", "armor piece")
    prefix("/Actor/Pack/Weapon_", gamefiles / "weapons", "weapon")


def check_version(py, gamefiles):
    # The 1.9.0 update's actor list is bigger than any earlier one.
    code = ("import sys, oead\n"
            "info = oead.byml.from_binary(oead.yaz0.decompress(open(sys.argv[1], 'rb').read()))\n"
            "sys.exit(0 if len(info['Actors']) >= 7900 else 1)\n")
    r = subprocess.run([str(py), "-I", "-c", code, str(gamefiles / "ActorInfo-1.9.0.sbyml")])
    if r.returncode != 0:
        fail("this doesn't look like BotW with the 1.9.0 update (the mod only runs on 1.9.0).")


def build_code():
    deploy = ROOT / "client" / "deploy"
    devkit = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    if (devkit / "devkitA64").is_dir() and (ROOT / "client" / "Makefile").is_file() and shutil.which("make"):
        say("Building the mod's code")
        env = dict(os.environ, DEVKITPRO=str(devkit))
        r = subprocess.run(["make", "-C", str(ROOT / "client")], env=env, capture_output=True, text=True)
        if r.returncode != 0:
            fail("the mod's code didn't build:\n" + "\n".join((r.stdout + r.stderr).splitlines()[-15:]))
        print("ok")
    elif (deploy / "subsdk9").is_file():
        say("Using the prebuilt mod code (client/deploy/subsdk9)")
    else:
        fail("the mod's code isn't here (client/deploy/subsdk9) and devkitPro isn't installed to build it. "
             "Use a release package, which includes it.")
    return deploy


def build_relay():
    """Returns the relay to run, or None."""
    out = ROOT / ".tools" / f"relay{EXE}"
    if shutil.which("go") and (ROOT / "server" / "main.go").is_file():
        say("Building the relay server")
        out.parent.mkdir(exist_ok=True)
        r = subprocess.run(["go", "build", "-o", str(out), "."], cwd=ROOT / "server")
        if r.returncode == 0:
            print(f"ok: {out}")
            return out
    for cand in (ROOT / "relay" / f"relay{EXE}", out):
        if cand.is_file():
            if not WINDOWS:
                cand.chmod(0o755)
            return cand
    return None


def copy_tree(src, dst):
    shutil.copytree(src, dst, dirs_exist_ok=True)


def write_config(path, server, backups, log_file):
    if path.is_file():
        print(f"Kept your settings: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(CONFIG_TEMPLATE.format(server=server, backups=backups, log_file=log_file), newline="\n")
    print(f"Wrote default settings: {path}")


def install_ryujinx(ryu, deploy, romfs):
    mod = ryu / "mods" / "contents" / TITLE / "botw-coop"
    (mod / "exefs").mkdir(parents=True, exist_ok=True)
    # Only the code goes in exefs: Ryujinx keeps the game's own main.npdm.
    shutil.copyfile(deploy / "subsdk9", mod / "exefs" / "subsdk9")
    copy_tree(romfs, mod / "romfs")
    config = mod / "romfs" / "botw-coop" / "config.ini"
    write_config(config, "127.0.0.1", 3, "false")
    print(f"Installed into {mod}")
    return config


def install_switch(out, deploy, romfs):
    # Atmosphere wants the title id in capitals; main.npdm is needed on a real
    # console (it lets the mod's hooks patch the game's code in memory).
    contents = out / "atmosphere" / "contents" / TITLE.upper()
    (contents / "exefs").mkdir(parents=True, exist_ok=True)
    if not (deploy / "main.npdm").is_file():
        fail("client/deploy/main.npdm is missing; a real Switch needs it.")
    shutil.copyfile(deploy / "subsdk9", contents / "exefs" / "subsdk9")
    shutil.copyfile(deploy / "main.npdm", contents / "exefs" / "main.npdm")
    copy_tree(romfs, contents / "romfs")
    config = contents / "romfs" / "botw-coop" / "config.ini"
    write_config(config, "192.168.1.2", 1, "true")
    print(f"Wrote the Switch files into {out / 'atmosphere'}")
    return config


def unquarantine():
    """macOS marks downloaded files; the programs in this package would be
    refused ("can't be opened"). Clears the mark on this folder only."""
    if sys.platform == "darwin" and shutil.which("xattr"):
        for sub in ("tools/bin", "relay"):
            if (ROOT / sub).is_dir():
                subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(ROOT / sub)], capture_output=True)


def main():
    ap = argparse.ArgumentParser(description="Build the BotW co-op mod from your own game dump and install it.")
    ap.add_argument("base", help="your base game (.nsp or .xci)")
    ap.add_argument("update", help="your 1.9.0 update (.nsp)")
    ap.add_argument("--switch", metavar="FOLDER", help="also write the files for a modded Switch into FOLDER "
                    "(an `atmosphere` folder to copy to the SD card; or give the SD card itself)")
    ap.add_argument("--ryujinx-dir", metavar="FOLDER", default=os.environ.get("RYU_PATH"),
                    help="Ryujinx's data folder, if it isn't in the usual place")
    ap.add_argument("--no-ryujinx", action="store_true", default=bool(os.environ.get("NO_INSTALL")),
                    help="don't install into Ryujinx")
    args = ap.parse_args()

    base, update = Path(args.base).expanduser(), Path(args.update).expanduser()
    if not base.is_file():
        fail(f"can't find the base game file: {base}")
    if not update.is_file():
        fail(f"can't find the update file: {update}")
    # The base game is far bigger than its update: given the other way round
    # (easy when dragging two files onto setup.bat), swap them.
    if base.stat().st_size < update.stat().st_size:
        print(f"Taking {update.name} as the base game and {base.name} as the update (by size).")
        base, update = update, base
    gamefiles = Path(os.environ.get("GAMEFILES", ROOT / ".gamefiles"))
    romfs = Path(os.environ.get("ROMFS_OUT", ROOT / "client" / "romfs"))

    say("Checking what's needed")
    unquarantine()
    check_keys()
    py = venv_python()
    tool = romfs_tool()
    print("ok")

    say("Reading your game dump (the first time takes a few minutes)")
    extract_files(tool, base, update, gamefiles)
    check_version(py, gamefiles)

    say("Building the puppet actors from your game files")
    r = subprocess.run([str(py), "-I", str(ROOT / "tools" / "build_romfs_mod.py"), str(gamefiles), str(romfs)])
    if r.returncode != 0:
        fail("building the puppet actors failed (see the messages above).")

    deploy = build_code()
    relay = build_relay()

    configs = []
    if not args.no_ryujinx:
        say("Installing into Ryujinx")
        ryu = Path(args.ryujinx_dir).expanduser() if args.ryujinx_dir else next(
            (d for d in ryujinx_dirs() if d.is_dir()), None)
        if ryu is None or not ryu.is_dir():
            print("Ryujinx's data folder wasn't found, so nothing was installed there. Start Ryujinx once, or "
                  "give the folder with --ryujinx-dir (in Ryujinx: File > Open Ryujinx Folder).")
        else:
            configs.append(install_ryujinx(ryu, deploy, romfs))
    if args.switch:
        say("Writing the Switch files")
        configs.append(install_switch(Path(args.switch).expanduser().resolve(), deploy, romfs))

    print("\nDone. Next:")
    for c in configs:
        print(f"  - Set your name, the server address and the password in:\n      {c}")
    if not args.no_ryujinx:
        print("  - In Ryujinx: Options > Settings > Network > turn on Guest Internet Access.")
    if args.switch:
        print("  - Copy the `atmosphere` folder onto your Switch's SD card (merge it with the one there).")
    if relay:
        print(f"  - One player starts the relay and leaves it running:\n      \"{relay}\"")
    else:
        print("  - One player needs the relay server (it's in the release package, or build server/ with Go).")
    print("  - How to play together: docs/PLAYING.md")


if __name__ == "__main__":
    main()
