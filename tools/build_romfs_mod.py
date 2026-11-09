"""Builds the mod's RomFS files from your own game files.

    .venv/bin/python -I tools/build_romfs_mod.py <gamefiles dir> <out romfs dir>

<gamefiles dir> must contain (extract them with tools/RomfsTool, base + 1.9.0 update):
    ActorInfo-1.9.0.sbyml                    Actor/ActorInfo.product.sbyml
    ResourceSizeTable.product.srsizetable    System/Resource/ResourceSizeTable.product.srsizetable
    packs/<template>.sbactorpack             Actor/Pack/<template>.sbactorpack for each template below

Output (merged into the game's RomFS by the emulator):
    Actor/Pack/<new>.sbactorpack
    Actor/ActorInfo.product.sbyml            1.9.0's list plus our actors
    System/Resource/ResourceSizeTable.product.srsizetable

Game files are never committed; this script recreates the derived files.
"""
import sys
import zlib
from pathlib import Path

import oead
import rstb

# Each new actor: copied from a template actor, with ActorLink fields changed.
ACTORS = [
    {
        # Stand-in for other players until the Link puppet: the Sheikah monk
        # without physics. Its physics body used to pull it back to where it
        # spawned; without one it stays wherever the mod puts it.
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
]


def set_link_params(pio: oead.aamp.ParameterIO, changes: dict) -> None:
    target = pio.objects["LinkTarget"]
    for key, value in changes.items():
        old = target.params[key].v
        if isinstance(old, float):
            target.params[key] = oead.aamp.Parameter(float(value))
        else:
            # Keep the same string type the file used.
            target.params[key] = oead.aamp.Parameter(type(old)(value) if not isinstance(old, str) else value)


def build_pack(template_pack: bytes, template: str, name: str, link_changes: dict) -> bytes:
    sarc = oead.Sarc(oead.yaz0.decompress(template_pack))
    writer = oead.SarcWriter.from_sarc(sarc)
    old_link = f"Actor/ActorLink/{template}.bxml"
    pio = oead.aamp.ParameterIO.from_binary(bytes(sarc.get_file(old_link).data))
    set_link_params(pio, link_changes)
    del writer.files[old_link]
    writer.files[f"Actor/ActorLink/{name}.bxml"] = pio.to_binary()
    _, data = writer.write()
    return bytes(data)


def add_actor_info(info: oead.byml.Hash, template: str, name: str, overrides: dict) -> None:
    actors = info["Actors"]
    hashes = info["Hashes"]
    names = [str(a["name"]) for a in actors]
    if name in names:
        raise SystemExit(f"{name} is already in ActorInfo")
    src = actors[names.index(template)]
    entry = oead.byml.Hash()
    for k, v in src.items():
        entry[k] = v
    entry["name"] = name
    for k, v in overrides.items():
        old = entry[k]
        entry[k] = oead.F32(v) if isinstance(old, (float, oead.F32)) else v

    # Actors are sorted by CRC32 of the name; Hashes is the parallel list.
    h = zlib.crc32(name.encode())
    keys = [int(x) for x in hashes]
    idx = 0
    while idx < len(keys) and keys[idx] < h:
        idx += 1
    new_actors = oead.byml.Array(list(actors)[:idx] + [entry] + list(actors)[idx:])
    hval = oead.U32(h) if h >= 0x80000000 else oead.S32(h)
    new_hashes = oead.byml.Array(list(hashes)[:idx] + [hval] + list(hashes)[idx:])
    info["Actors"] = new_actors
    info["Hashes"] = new_hashes


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    calc = rstb.SizeCalculator()

    rstb_raw = oead.yaz0.decompress((src / "ResourceSizeTable.product.srsizetable").read_bytes())
    table = rstb.ResourceSizeTable(bytes(rstb_raw), be=False)

    info_raw = oead.yaz0.decompress((src / "ActorInfo-1.9.0.sbyml").read_bytes())
    byml_version = int.from_bytes(bytes(info_raw[2:4]), "little")
    info = oead.byml.from_binary(info_raw)

    (out / "Actor/Pack").mkdir(parents=True, exist_ok=True)
    (out / "System/Resource").mkdir(parents=True, exist_ok=True)

    for actor in ACTORS:
        name, template = actor["name"], actor["template"]
        pack = build_pack((src / "packs" / f"{template}.sbactorpack").read_bytes(), template, name, actor["link"])
        (out / f"Actor/Pack/{name}.sbactorpack").write_bytes(bytes(oead.yaz0.compress(pack)))

        # The game sizes its load buffers from this table; new files need entries.
        pack_size = calc.calculate_file_size_with_ext(pack, False, ".bactorpack")
        tmpl_pack = f"Actor/Pack/{template}.bactorpack"
        if table.is_in_table(tmpl_pack):
            pack_size = max(pack_size, table.get_size(tmpl_pack))
        table.set_size(f"Actor/Pack/{name}.bactorpack", pack_size)
        tmpl_link = f"Actor/ActorLink/{template}.bxml"
        link_size = table.get_size(tmpl_link) if table.is_in_table(tmpl_link) else 4096
        table.set_size(f"Actor/ActorLink/{name}.bxml", link_size)

        add_actor_info(info, template, name, actor.get("info", {}))
        print(f"{name}: pack {len(pack)} bytes (table size {pack_size}), from {template}")

    info_bin = oead.byml.to_binary(info, big_endian=False, version=byml_version)
    (out / "Actor/ActorInfo.product.sbyml").write_bytes(bytes(oead.yaz0.compress(info_bin)))
    print(f"ActorInfo: {len(info['Actors'])} actors, {len(info_bin)} bytes")

    import io
    buf = io.BytesIO()
    table.write(buf, be=False)
    (out / "System/Resource/ResourceSizeTable.product.srsizetable").write_bytes(
        bytes(oead.yaz0.compress(buf.getvalue())))
    print("ResourceSizeTable written")


if __name__ == "__main__":
    main()
