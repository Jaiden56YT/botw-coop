"""Read single files out of the base game's RomFS, and decode Yaz0 + BYML.

    .venv/bin/python -I tools/romfs.py <base.nsp> extract <romfs path> <out file>
    .venv/bin/python -I tools/romfs.py <base.nsp> list [substring]
    .venv/bin/python -I tools/romfs.py - byml <file.sbyml|byml> [json out]

Uses the keys in ~/.switch/prod.keys. Only the base game's RomFS is read
(update RomFS is a patch and isn't supported).
"""
import json
import struct
import sys
from pathlib import Path


def open_romfs(nsp_path):
    from nsz.nut import Keys
    Keys.load(str(Path.home() / ".switch" / "prod.keys"))
    from nsz.Fs import Nsp, Nca, Rom
    from nsz.Fs.Type import Content

    nsp = Nsp.Nsp(str(nsp_path), "rb")
    for f in nsp:
        if isinstance(f, Nca.Nca) and f.header.contentType == Content.PROGRAM:
            for fs in f:
                if isinstance(fs, Rom.Rom):
                    return RomFs(fs)
    raise SystemExit("no RomFS in program NCA")


class RomFs:
    def __init__(self, sec):
        self.sec = sec
        lvl = sec.ivfc.levels[-1]
        self.base = lvl.offset
        hdr = self.read(0, 0x50)
        (_, _dh_off, _dh_size, self.dm_off, self.dm_size, _fh_off, _fh_size,
         self.fm_off, self.fm_size, self.data_off) = struct.unpack_from("<10Q", hdr)
        self.dir_meta = self.read(self.dm_off, self.dm_size)
        self.file_meta = self.read(self.fm_off, self.fm_size)

    def read(self, off, size):
        self.sec.seek(self.base + off)
        return self.sec.read(size)

    def _dir_path(self, off):
        parts = []
        while off != 0:
            parent, = struct.unpack_from("<I", self.dir_meta, off)
            name_len, = struct.unpack_from("<I", self.dir_meta, off + 0x14)
            parts.append(self.dir_meta[off + 0x18: off + 0x18 + name_len].decode())
            off = parent
        return "/".join(reversed(parts))

    def files(self):
        off = 0
        while off < len(self.file_meta):
            parent, _sib, data_off, size, _hash, name_len = struct.unpack_from("<IIQQII", self.file_meta, off)
            name = self.file_meta[off + 0x20: off + 0x20 + name_len].decode()
            d = self._dir_path(parent)
            yield (f"{d}/{name}" if d else name), data_off, size
            off += 0x20 + ((name_len + 3) & ~3)

    def get(self, path):
        for p, data_off, size in self.files():
            if p == path:
                return self.read(self.data_off + data_off, size)
        raise KeyError(path)


def yaz0(data):
    if data[:4] != b"Yaz0":
        return data
    size, = struct.unpack_from(">I", data, 4)
    out = bytearray()
    src = 16
    while len(out) < size:
        code = data[src]; src += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(data[src]); src += 1
            else:
                b1, b2 = data[src], data[src + 1]; src += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12; src += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def byml(data):
    le = data[:2] == b"YB"
    e = "<" if le else ">"
    u32 = lambda o: struct.unpack_from(e + "I", data, o)[0]
    u16 = lambda o: struct.unpack_from(e + "H", data, o)[0]

    def u24(o):
        b = data[o:o + 3]
        return int.from_bytes(b, "little" if le else "big")

    def strtab(off):
        if off == 0:
            return []
        n = u24(off + 1)
        return [data[off + u32(off + 4 + 4 * i):].split(b"\0", 1)[0].decode("utf-8", "replace") for i in range(n)]

    keys = strtab(u32(4))
    strs = strtab(u32(8))
    root = u32(12)

    def value(t, raw):
        if t == 0xA0: return strs[raw]
        if t in (0xC0, 0xC1): return node(raw)
        if t == 0xD0: return bool(raw)
        if t == 0xD1: return struct.unpack(e + "i", struct.pack(e + "I", raw))[0]
        if t == 0xD2: return struct.unpack(e + "f", struct.pack(e + "I", raw))[0]
        if t == 0xD3: return raw
        if t == 0xFF: return None
        return f"<type {t:#x}>"

    def node(off):
        t = data[off]
        n = u24(off + 1)
        if t == 0xC0:
            types = data[off + 4: off + 4 + n]
            vo = off + 4 + ((n + 3) & ~3)
            return [value(types[i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                eo = off + 4 + 8 * i
                k = u24(eo)
                out[keys[k]] = value(data[eo + 3], u32(eo + 4))
            return out
        return f"<node {t:#x}>"

    return node(root) if root else None


def main():
    if sys.argv[2] == "byml":
        obj = byml(yaz0(open(sys.argv[3], "rb").read()))
        text = json.dumps(obj, ensure_ascii=False, indent=1)
        if len(sys.argv) > 4:
            open(sys.argv[4], "w").write(text)
        else:
            print(text[:4000])
        return

    fs = open_romfs(sys.argv[1])
    if sys.argv[2] == "list":
        sub = sys.argv[3] if len(sys.argv) > 3 else ""
        for p, _o, size in fs.files():
            if sub in p:
                print(size, p)
    elif sys.argv[2] == "extract":
        data = fs.get(sys.argv[3])
        open(sys.argv[4], "wb").write(data)
        print(f"wrote {len(data)} bytes")


if __name__ == "__main__":
    main()
