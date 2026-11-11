"""Builds the mod's RomFS files from your own game files.

    .venv/bin/python -I tools/build_romfs_mod.py <gamefiles dir> <out romfs dir>

<gamefiles dir> must contain (extract them with tools/RomfsTool, base + 1.9.0 update):
    ActorInfo-1.9.0.sbyml                    Actor/ActorInfo.product.sbyml
    ResourceSizeTable.product.srsizetable    System/Resource/ResourceSizeTable.product.srsizetable
    packs/<actor>.sbactorpack                Actor/Pack/<actor>.sbactorpack for every template below

Output (merged into the game's RomFS by the emulator):
    Actor/Pack/<new>.sbactorpack
    Actor/ActorInfo.product.sbyml            1.9.0's list plus our actors
    System/Resource/ResourceSizeTable.product.srsizetable

Game files are never committed; this script recreates the derived files.
"""
import copy
import io
import re
import sys
import zlib
from pathlib import Path

import oead
import rstb

# Each new actor starts as a copy of `template`'s pack. Optional parts:
#   borrow:   {source actor: [file name prefixes]} files copied in from other packs
#   rename:   {path in pack: new path}  (applied after borrowing)
#   aslist:   (path, {AS name: AS file})  rewrites an ASList's defines, keeping its AddReses
#   link:     ActorLink fields to change
#   info / info_from:  ActorInfo fields to set / copy from another actor's entry
ACTORS = [
    {
        # Stand-in for other players: the Sheikah monk without physics. Its
        # physics body pulled it back to where it spawned; without one it
        # stays wherever the mod puts it.
        "name": "CoopGhost",
        "template": "Npc_DungeonPriest001",
        "link": {
            "ActorNameJpn": "協力プレイヤー",
            "PhysicsUser": "Dummy",
            "AttentionUser": "Dummy",  # no "Talk" prompt
            "ChemicalUser": "Dummy",
        },
        "info": {"actorScale": 1.0},
    },
    {
        # The other player as Link. The monk's cutscene-NPC setup (its AI
        # loops the AS named "Wait"), with Link's model and animation
        # sequences from the pause-menu Link, and no physics.
        "name": "CoopPlayer",
        "template": "Npc_DungeonPriest001",
        "borrow": {"PauseMenuPlayer": ["Actor/AS/", "Actor/ModelList/", "Actor/ASList/"]},
        "rename": {
            "Actor/ModelList/PauseMenuPlayer.bmodellist": "Actor/ModelList/CoopPlayer.bmodellist",
            "Actor/ASList/PauseMenuPlayer.baslist": "Actor/ASList/CoopPlayer.baslist",
        },
        # One-clip animation sequences we generate (the stock idle is a selector
        # keyed on the real player's weapon and temperature, which leaves a
        # puppet in a T-pose).
        # The mod switches between these from the other player's movement.
        "clips": {
            "Actor/AS/CoopPlayer_Wait.bas": "Nml_Wait",
            "Actor/AS/CoopPlayer_Walk.bas": "Nml_Move_Walk",
            "Actor/AS/CoopPlayer_Run.bas": "Nml_Move_Run",
            "Actor/AS/CoopPlayer_Dash.bas": "Nml_Move_Dash",
            "Actor/AS/CoopPlayer_Fall.bas": "Fall",
            "Actor/AS/CoopPlayer_Glide.bas": "Float_Float_F",
            "Actor/AS/CoopPlayer_Swim.bas": "Swim_Move_Slow_F",
            "Actor/AS/CoopPlayer_SwimWait.bas": "Swim_Wait",
            "Actor/AS/CoopPlayer_SwimDash.bas": "Swim_Move_Fast_F",
            "Actor/AS/CoopPlayer_Climb.bas": "Climb_Wall_Move_U",
            "Actor/AS/CoopPlayer_ClimbWait.bas": "Climb_Wall_Wait_FootL",
            "Actor/AS/CoopPlayer_Jump.bas": "Nml_Move_Run_Jump_FootL",
            # Full-body riding clips (the A_Pose_Horse_* ones are partial poses).
            "Actor/AS/CoopPlayer_Horse.bas": "Horse_Wait",
            "Actor/AS/CoopPlayer_HorseGear1.bas": "Horse_Move_Gear_1",
            "Actor/AS/CoopPlayer_HorseGear2.bas": "Horse_Move_Gear_2",
            "Actor/AS/CoopPlayer_HorseGear3.bas": "Horse_Move_Gear_3",
            "Actor/AS/CoopPlayer_Surf.bas": "Shield_Board_Go2",
            # Down, waiting for a partner (co-op revive): Link's collapse.
            "Actor/AS/CoopPlayer_Down.bas": "Nml_Die",
            # Actions (attacks, bow, throw, guard), from the player's own clips.
            "Actor/AS/CoopPlayer_Attack1.bas": "Sword_Attack_S1",
            "Actor/AS/CoopPlayer_Attack2.bas": "Sword_Attack_S2",
            "Actor/AS/CoopPlayer_Attack3.bas": "Sword_Attack_S3",
            "Actor/AS/CoopPlayer_AttackF.bas": "Sword_Attack_SF",
            "Actor/AS/CoopPlayer_Spin.bas": "Sword_Attack_Charge",
            "Actor/AS/CoopPlayer_ChargeWait.bas": "Sword_Charge_Wait",
            "Actor/AS/CoopPlayer_BowAim.bas": "Bow_Attack_Charge",
            "Actor/AS/CoopPlayer_BowShoot.bas": "Bow_Attack_Shoot",
            "Actor/AS/CoopPlayer_ThrowAim.bas": "Sword_Throw_Charge_Wait",
            "Actor/AS/CoopPlayer_Throw.bas": "Sword_Throw_Throw",
            "Actor/AS/CoopPlayer_Guard.bas": "Shield_Guard_Wait",
        },
        # The AI plays "Wait" into animation slot 0. The pause-menu Link's model
        # list (kept as-is) has slot 0 as the whole skeleton; the monk's slot 0
        # is the face only.
        "aslist": ("Actor/ASList/CoopPlayer.baslist", {
            "Wait": "CoopPlayer_Wait",
            "Walk": "CoopPlayer_Walk",
            "Run": "CoopPlayer_Run",
            "Dash": "CoopPlayer_Dash",
            "Fall": "CoopPlayer_Fall",
            "Glide": "CoopPlayer_Glide",
            "Swim": "CoopPlayer_Swim",
            "SwimWait": "CoopPlayer_SwimWait",
            "SwimDash": "CoopPlayer_SwimDash",
            "Climb": "CoopPlayer_Climb",
            "ClimbWait": "CoopPlayer_ClimbWait",
            "Jump": "CoopPlayer_Jump",
            "Horse": "CoopPlayer_Horse",
            "HorseGear1": "CoopPlayer_HorseGear1",
            "HorseGear2": "CoopPlayer_HorseGear2",
            "HorseGear3": "CoopPlayer_HorseGear3",
            "Surf": "CoopPlayer_Surf",
            "Down": "CoopPlayer_Down",
            "Attack1": "CoopPlayer_Attack1",
            "Attack2": "CoopPlayer_Attack2",
            "Attack3": "CoopPlayer_Attack3",
            "AttackF": "CoopPlayer_AttackF",
            "Spin": "CoopPlayer_Spin",
            "ChargeWait": "CoopPlayer_ChargeWait",
            "BowAim": "CoopPlayer_BowAim",
            "BowShoot": "CoopPlayer_BowShoot",
            "ThrowAim": "CoopPlayer_ThrowAim",
            "Throw": "CoopPlayer_Throw",
            "Guard": "CoopPlayer_Guard",
            "FaceDefault": "Player_FaceDefault",
            "SkinColor": "Player_SkinColor",
            "PauseMenuWait": "Player_PauseMenuWait",
        }),
        "drop": ["Actor/ModelList/Npc_DungeonPriest001.bmodellist", "Actor/ASList/Npc_DungeonPriest001.baslist",
                 "Actor/AS/Npc_DungeonPriest001_Wait.bas", "Actor/AS/Npc_DungeonPriest001_Vanish.bas",
                 "Physics/SupportBone/Npc_Shiekah_Maharishi/Npc_Shiekah_Maharishi.bphyssb",
                 "Actor/Physics/Npc_DungeonPriest001.bphysics"],
        "link": {
            "ActorNameJpn": "協力プレイヤー(リンク)",
            "ModelUser": "CoopPlayer",
            "ASUser": "CoopPlayer",
            "PhysicsUser": "Dummy",
            "AttentionUser": "Dummy",
            "ChemicalUser": "Dummy",
            "ElinkUser": "Dummy",
            "SlinkUser": "Dummy",
            "XlinkUser": "Dummy",
            "ActorScale": 1.0,
        },
        # instSize is the actor's own heap. The monk's (326,640) is too small
        # for Link's model plus its animation state, which left the puppet in
        # a T-pose; the pause-menu Link uses 557,256. Add headroom.
        "info": {"actorScale": 1.0, "elink": "Dummy", "slink": "Dummy", "xlink": "Dummy", "instSize": 700000},
        "info_from": ("PauseMenuPlayer", ["bfres", "mainModel", "aabbMin", "aabbMax", "boundingForTraverse",
                                          "rigidBodyCenterY", "lookAtOffsetY", "cursorOffsetY"]),
    },
    {
        # A horse under a partner who's riding: the monk's physics-free NPC
        # setup with a horse's model and animation sequences. The mod plays
        # Wait / Move_Gear1-4 from the rider's speed.
        "name": "CoopHorse",
        "template": "Npc_DungeonPriest001",
        "borrow": {"GameRomHorse01": ["Actor/AS/", "Actor/ModelList/", "Actor/ASList/"]},
        "drop": ["Actor/ModelList/Npc_DungeonPriest001.bmodellist", "Actor/ASList/Npc_DungeonPriest001.baslist",
                 "Actor/AS/Npc_DungeonPriest001_Wait.bas", "Actor/AS/Npc_DungeonPriest001_Vanish.bas",
                 "Physics/SupportBone/Npc_Shiekah_Maharishi/Npc_Shiekah_Maharishi.bphyssb",
                 "Actor/Physics/Npc_DungeonPriest001.bphysics"],
        "link": {
            "ActorNameJpn": "協力プレイヤーの馬",
            "ModelUser": "EngineRom_Horse_Link",
            "ASUser": "Horse_Link",
            "PhysicsUser": "Dummy",
            "AttentionUser": "Dummy",
            "ChemicalUser": "Dummy",
            "ElinkUser": "Dummy",
            "SlinkUser": "Dummy",
            "XlinkUser": "Dummy",
            "ActorScale": 1.0,
        },
        "info": {"actorScale": 1.0, "elink": "Dummy", "slink": "Dummy", "xlink": "Dummy", "instSize": 700000},
        "info_from": ("GameRomHorse01", ["bfres", "mainModel", "aabbMin", "aabbMax", "boundingForTraverse"]),
    },
]



def set_link_params(pio: oead.aamp.ParameterIO, changes: dict) -> None:
    target = pio.objects["LinkTarget"]
    for key, value in changes.items():
        old = target.params[key].v
        if isinstance(old, float):
            target.params[key] = oead.aamp.Parameter(float(value))
        elif isinstance(old, str):
            target.params[key] = oead.aamp.Parameter(value)
        else:
            target.params[key] = oead.aamp.Parameter(type(old)(value))


def rewrite_aslist(data: bytes, defines: dict) -> bytes:
    pio = oead.aamp.ParameterIO.from_binary(data)
    defs = oead.aamp.ParameterList()
    # The game finds sequences by binary search on the name, so the list must
    # be sorted; out of order, names silently fail to resolve (T-pose).
    for i, (name, filename) in enumerate(sorted(defines.items())):
        obj = oead.aamp.ParameterObject()
        obj.params["Name"] = oead.aamp.Parameter(oead.FixedSafeString64(name))
        obj.params["Filename"] = oead.aamp.Parameter(oead.FixedSafeString64(filename))
        defs.objects[f"ASDefine_{i}"] = obj
    pio.lists["ASDefines"] = defs
    return bytes(pio.to_binary())


def clip_as(clip: str) -> bytes:
    """An animation sequence that just plays one skeletal clip (element type 67)."""
    text = f"""!io
version: 0
type: xml
param_root: !list
  objects: {{}}
  lists:
    Elements: !list
      objects: {{}}
      lists:
        Element0: !list
          objects:
            Parameters: !obj
              TypeIndex: 67
              FileName: !str64 {clip}
              Morph: 0.0
          lists: {{}}
"""
    return bytes(oead.aamp.ParameterIO.from_text(text).to_binary())


def build_pack(src: Path, actor: dict) -> bytes:
    name, template = actor["name"], actor["template"]
    sarc = oead.Sarc(oead.yaz0.decompress((src / "packs" / f"{template}.sbactorpack").read_bytes()))
    writer = oead.SarcWriter.from_sarc(sarc)

    for other, prefixes in actor.get("borrow", {}).items():
        osarc = oead.Sarc(oead.yaz0.decompress((src / "packs" / f"{other}.sbactorpack").read_bytes()))
        for f in osarc.get_files():
            if any(f.name.startswith(p) for p in prefixes):
                writer.files[f.name] = bytes(f.data)

    for old, new in actor.get("rename", {}).items():
        writer.files[new] = bytes(writer.files[old])
        del writer.files[old]

    for path in actor.get("drop", []):
        if path in writer.files:
            del writer.files[path]

    if "anm_targets_from" in actor:
        path, other, other_path = actor["anm_targets_from"]
        osarc = oead.Sarc(oead.yaz0.decompress((src / "packs" / f"{other}.sbactorpack").read_bytes()))
        donor = oead.aamp.ParameterIO.from_binary(bytes(osarc.get_file(other_path).data))
        pio = oead.aamp.ParameterIO.from_binary(bytes(writer.files[path]))
        pio.lists["AnmTarget"] = donor.lists["AnmTarget"]
        writer.files[path] = bytes(pio.to_binary())

    for path, clip in actor.get("clips", {}).items():
        writer.files[path] = clip_as(clip)

    if "aslist" in actor:
        path, defines = actor["aslist"]
        writer.files[path] = rewrite_aslist(bytes(writer.files[path]), defines)

    old_link = f"Actor/ActorLink/{template}.bxml"
    pio = oead.aamp.ParameterIO.from_binary(bytes(sarc.get_file(old_link).data))
    set_link_params(pio, actor.get("link", {}))
    del writer.files[old_link]
    writer.files[f"Actor/ActorLink/{name}.bxml"] = pio.to_binary()

    _, data = writer.write()
    return bytes(data)


def add_actor_info(info: oead.byml.Hash, actor: dict) -> None:
    name, template = actor["name"], actor["template"]
    actors = info["Actors"]
    hashes = info["Hashes"]
    by_name = {str(a["name"]): a for a in actors}
    if name in by_name:
        raise SystemExit(f"{name} is already in ActorInfo")
    entry = oead.byml.Hash()
    for k, v in by_name[template].items():
        entry[k] = v
    entry["name"] = name
    if "info_from" in actor:
        other, keys = actor["info_from"]
        for k in keys:
            if k in by_name[other]:
                entry[k] = by_name[other][k]
    for k, v in actor.get("info", {}).items():
        old = entry[k] if k in entry else None
        if isinstance(old, (float, oead.F32)) or isinstance(v, float):
            entry[k] = oead.F32(v)
        elif isinstance(old, oead.U32):
            entry[k] = oead.U32(v)
        elif isinstance(v, int) and not isinstance(v, bool):
            entry[k] = oead.S32(v)  # BYML ints must use oead's own types
        else:
            entry[k] = v

    # Actors are sorted by CRC32 of the name; Hashes is the parallel list.
    h = zlib.crc32(name.encode())
    keys = [int(x) for x in hashes]
    idx = 0
    while idx < len(keys) and keys[idx] < h:
        idx += 1
    hval = oead.U32(h) if h >= 0x80000000 else oead.S32(h)
    info["Actors"] = oead.byml.Array(list(actors)[:idx] + [entry] + list(actors)[idx:])
    info["Hashes"] = oead.byml.Array(list(hashes)[:idx] + [hval] + list(hashes)[idx:])


# Progress flags shared between players (only ever set, never cleared).
SYNC_FLAG_PATTERNS = [
    r"Clear_Dungeon\d+",       # shrine completed
    r"Open_Dungeon\d+",        # shrine activated
    r"Enter_Dungeon\d+",       # shrine found
    r"MapTower_\d+",           # tower activated (map region revealed)
    r"Clear_Remains\w+",       # Divine Beast cleared
    r"MainField_Npc_HiddenKorok(Ground|Fly)_\d+",  # Korok found
]


def write_sync_flags(bootup_pack: Path, out_file: Path) -> int:
    """Lists the bool game-data flags the mod shares, from GameData/gamedata.ssarc."""
    boot = oead.Sarc(bootup_pack.read_bytes())
    gd = oead.Sarc(oead.yaz0.decompress(bytes(boot.get_file("GameData/gamedata.ssarc").data)))
    rx = re.compile("^(" + "|".join(SYNC_FLAG_PATTERNS) + ")$")
    names = set()
    for f in gd.get_files():
        y = oead.byml.from_binary(bytes(f.data))
        for e in y.get("bool_data", []):
            n = str(e["DataName"])
            if rx.match(n):
                names.add(n)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text("\n".join(sorted(names)) + "\n", newline="\n")
    return len(names)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    if (src / "Bootup.pack").exists():
        n = write_sync_flags(src / "Bootup.pack", out / "botw-coop" / "sync-flags.txt")
        print(f"sync-flags.txt: {n} flags")
    calc = rstb.SizeCalculator()

    table = rstb.ResourceSizeTable(
        bytes(oead.yaz0.decompress((src / "ResourceSizeTable.product.srsizetable").read_bytes())), be=False)
    info_raw = oead.yaz0.decompress((src / "ActorInfo-1.9.0.sbyml").read_bytes())
    byml_version = int.from_bytes(bytes(info_raw[2:4]), "little")
    info = oead.byml.from_binary(info_raw)

    (out / "Actor/Pack").mkdir(parents=True, exist_ok=True)
    (out / "System/Resource").mkdir(parents=True, exist_ok=True)

    built = {}

    def add_pack(name: str, template: str, pack: bytes) -> oead.Sarc:
        (out / f"Actor/Pack/{name}.sbactorpack").write_bytes(bytes(oead.yaz0.compress(pack)))

        # The game sizes its load buffers from this table; every new file name
        # in the pack needs an entry. Use the template's numbers where they're
        # bigger than the calculated ones.
        pack_size = calc.calculate_file_size_with_ext(pack, False, ".bactorpack")
        tmpl_pack = f"Actor/Pack/{template}.bactorpack"
        if table.is_in_table(tmpl_pack):
            pack_size = max(pack_size, table.get_size(tmpl_pack))
        table.set_size(f"Actor/Pack/{name}.bactorpack", pack_size)

        sarc = oead.Sarc(pack)
        for f in sarc.get_files():
            if table.is_in_table(f.name):
                continue
            ext = "." + f.name.rsplit(".", 1)[-1]
            size = calc.calculate_file_size_with_ext(bytes(f.data), False, ext)
            # Fall back to a generous estimate for types the calculator doesn't know.
            if size <= 0:
                size = len(f.data) * 4 + 0x1000
            table.set_size(f.name, size)
        return sarc

    for actor in ACTORS:
        name, template = actor["name"], actor["template"]
        pack = build_pack(src, actor)
        built[name] = pack
        sarc = add_pack(name, template, pack)
        add_actor_info(info, actor)
        print(f"{name}: pack {len(pack)} bytes, {len(list(sarc.get_files()))} files, from {template}")

    # Gear on the puppet: one actor per armor model, the CoopPlayer puppet
    # (same animation sequences, so it moves with the puppet) showing that
    # model. Many armors share a model (upgrades), so gear-map.txt maps each
    # armor actor name to its model.
    armor_dir = src / "armor"
    if armor_dir.exists():
        models, mapping = {}, []
        for path in sorted(armor_dir.glob("Armor_*.sbactorpack")):
            armor = path.name[: -len(".sbactorpack")]
            asarc = oead.Sarc(oead.yaz0.decompress(path.read_bytes()))
            lists = [f for f in asarc.get_files() if f.name.startswith("Actor/ModelList/")]
            if len(lists) != 1:
                continue
            model = lists[0].name.split("/")[-1][: -len(".bmodellist")]
            mapping.append((armor, model))
            models.setdefault(model, (armor, bytes(lists[0].data)))
        for model, (armor, model_list) in models.items():
            gname = f"CoopGear_{model}"
            writer = oead.SarcWriter.from_sarc(oead.Sarc(built["CoopPlayer"]))
            del writer.files["Actor/ModelList/CoopPlayer.bmodellist"]
            writer.files[f"Actor/ModelList/{model}.bmodellist"] = model_list
            link = oead.aamp.ParameterIO.from_binary(bytes(writer.files["Actor/ActorLink/CoopPlayer.bxml"]))
            set_link_params(link, {"ModelUser": model, "ActorNameJpn": "協力プレイヤー(装備)"})
            del writer.files["Actor/ActorLink/CoopPlayer.bxml"]
            writer.files[f"Actor/ActorLink/{gname}.bxml"] = link.to_binary()
            _, data = writer.write()
            add_pack(gname, "CoopPlayer", bytes(data))
            add_actor_info(info, {"name": gname, "template": "CoopPlayer",
                                  "info_from": (armor, ["bfres", "mainModel", "aabbMin", "aabbMax",
                                                        "boundingForTraverse"])})
        gear_map = out / "botw-coop" / "gear-map.txt"
        gear_map.parent.mkdir(parents=True, exist_ok=True)
        gear_map.write_text("".join(f"{a} {m}\n" for a, m in sorted(mapping)), newline="\n")
        print(f"gear: {len(models)} gear actors for {len(mapping)} armor pieces")

    # Weapons on the puppet: one actor per weapon model, showing just that
    # model. The mod binds it to a bone of the puppet (ModelBindInfo), so it
    # follows the hand or back. weapon-map.txt maps weapon actor -> model.
    weapon_dir = src / "weapons"
    if weapon_dir.exists():
        wmodels, wmapping = {}, []
        for path in sorted(weapon_dir.glob("Weapon_*.sbactorpack")):
            weapon = path.name[: -len(".sbactorpack")]
            wsarc = oead.Sarc(oead.yaz0.decompress(path.read_bytes()))
            lists = [f for f in wsarc.get_files() if f.name.startswith("Actor/ModelList/")]
            if len(lists) != 1:
                continue
            model = lists[0].name.split("/")[-1][: -len(".bmodellist")]
            wmapping.append((weapon, model))
            wmodels.setdefault(model, (weapon, bytes(lists[0].data)))
        for model, (weapon, model_list) in wmodels.items():
            wname = f"CoopWeapon_{model}"
            writer = oead.SarcWriter.from_sarc(oead.Sarc(built["CoopPlayer"]))
            del writer.files["Actor/ModelList/CoopPlayer.bmodellist"]
            writer.files[f"Actor/ModelList/{model}.bmodellist"] = model_list
            link = oead.aamp.ParameterIO.from_binary(bytes(writer.files["Actor/ActorLink/CoopPlayer.bxml"]))
            set_link_params(link, {"ModelUser": model, "ActorNameJpn": "協力プレイヤー(武器)"})
            del writer.files["Actor/ActorLink/CoopPlayer.bxml"]
            writer.files[f"Actor/ActorLink/{wname}.bxml"] = link.to_binary()
            _, data = writer.write()
            add_pack(wname, "CoopPlayer", bytes(data))
            add_actor_info(info, {"name": wname, "template": "CoopPlayer",
                                  "info_from": (weapon, ["bfres", "mainModel", "aabbMin", "aabbMax",
                                                         "boundingForTraverse"])})
        weapon_map = out / "botw-coop" / "weapon-map.txt"
        weapon_map.write_text("".join(f"{w} {m}\n" for w, m in sorted(wmapping)), newline="\n")
        print(f"weapons: {len(wmodels)} weapon actors for {len(wmapping)} weapons")

    info_bin = oead.byml.to_binary(info, big_endian=False, version=byml_version)
    (out / "Actor/ActorInfo.product.sbyml").write_bytes(bytes(oead.yaz0.compress(info_bin)))
    print(f"ActorInfo: {len(info['Actors'])} actors")

    buf = io.BytesIO()
    table.write(buf, be=False)
    (out / "System/Resource/ResourceSizeTable.product.srsizetable").write_bytes(
        bytes(oead.yaz0.compress(buf.getvalue())))
    print("ResourceSizeTable written")


if __name__ == "__main__":
    main()
