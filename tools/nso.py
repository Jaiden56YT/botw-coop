"""Decompress a Switch NSO into a flat memory image (text, rodata, data at their load offsets).

    .venv/bin/python -I tools/nso.py <main> <out.bin>

Prints the build ID and the segment layout so addresses in the image match
addresses relative to the module base at runtime.
"""
import struct
import sys

import lz4.block


def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"NSO0", "not an NSO"
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = []
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from("<III", d, 0x10 + i * 0x10)
        comp_size = struct.unpack_from("<I", d, 0x60 + i * 4)[0]
        raw = d[file_off:file_off + comp_size]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        segs.append((mem_off, raw))
    bss_size = struct.unpack_from("<I", d, 0x3C)[0]
    build_id = d[0x40:0x60]
    return segs, bss_size, build_id


def main():
    segs, bss, build_id = load(sys.argv[1])
    end = max(off + len(raw) for off, raw in segs)
    img = bytearray(end)
    for (off, raw), name in zip(segs, ("text", "rodata", "data")):
        img[off:off + len(raw)] = raw
        print(f"{name:6} 0x{off:08x}-0x{off + len(raw):08x}")
    print(f"bss    size 0x{bss:x}")
    print("build id", build_id.hex())
    open(sys.argv[2], "wb").write(img)


if __name__ == "__main__":
    main()
