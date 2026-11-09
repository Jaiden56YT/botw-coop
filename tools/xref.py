"""Tiny AArch64 cross-reference helpers for a flat NSO image (see nso.py).

    .venv/bin/python -I tools/xref.py <image.bin> str <text>       # code that loads a string
    .venv/bin/python -I tools/xref.py <image.bin> addr <hex>       # code that computes an address
    .venv/bin/python -I tools/xref.py <image.bin> calls <hex>      # BL instructions that call a function
    .venv/bin/python -I tools/xref.py <image.bin> func <hex>       # start of the function containing an address

Addresses are offsets from the module base. Only the patterns compilers use
for globals are recognised: ADRP followed (within a few instructions) by
ADD/LDR/STR on the same register.
"""
import struct
import sys

TEXT_END_GUESS = None


def load(path):
    data = open(path, "rb").read()
    return data


def words(data, end):
    return struct.unpack_from("<%dI" % (end // 4), data, 0)


def adrp_target(insn, pc):
    if (insn & 0x9F000000) != 0x90000000:
        return None
    immlo = (insn >> 29) & 3
    immhi = (insn >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~0xFFF) + (imm << 12), insn & 0x1F


def lo12_use(insn, reg):
    """Returns (kind, lo12) if insn adds/loads/stores using reg as base."""
    rn = (insn >> 5) & 0x1F
    if rn != reg:
        return None
    # ADD (immediate), 64-bit
    if (insn & 0xFF800000) == 0x91000000:
        shift = (insn >> 22) & 1
        imm = (insn >> 10) & 0xFFF
        return ("add", imm << (12 * shift))
    # LDR/STR (unsigned immediate), any size
    if (insn & 0x3B000000) == 0x39000000:
        size = (insn >> 30) & 3
        v = (insn >> 26) & 1
        opc = (insn >> 22) & 3
        scale = size
        if v and opc >= 2:
            scale = 4
        imm = ((insn >> 10) & 0xFFF) << scale
        kind = "ldr" if opc & 1 or (v and opc == 3) else "str"
        return (kind, imm)
    return None


def find_refs(ws, target, window=8):
    hits = []
    for i, insn in enumerate(ws):
        r = adrp_target(insn, i * 4)
        if not r:
            continue
        page, reg = r
        if page != (target & ~0xFFF):
            continue
        for j in range(i + 1, min(i + 1 + window, len(ws))):
            u = lo12_use(ws[j], reg)
            if u and page + u[1] == target:
                hits.append((i * 4, u[0]))
                break
    return hits


def find_calls(ws, target):
    out = []
    for i, insn in enumerate(ws):
        if (insn & 0xFC000000) == 0x94000000:
            imm = insn & 0x3FFFFFF
            if imm & (1 << 25):
                imm -= 1 << 26
            if i * 4 + imm * 4 == target:
                out.append(i * 4)
    return out


def func_start(ws, addr):
    """Walks back to the previous RET/B/BRK boundary; heuristic."""
    i = addr // 4 - 1
    while i > 0:
        insn = ws[i]
        if insn == 0xD65F03C0 or (insn & 0xFFE0001F) == 0xD4200000:  # ret / brk
            return (i + 1) * 4
        i -= 1
    return 0


def main():
    path, cmd, arg = sys.argv[1], sys.argv[2], sys.argv[3]
    data = load(path)
    text_end = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x015A1C64
    ws = words(data, text_end & ~3)
    if cmd == "str":
        needle = arg.encode() + b"\0"
        idx = data.find(needle)
        while idx >= 0 and idx > 0 and data[idx - 1] != 0:
            idx = data.find(needle, idx + 1)
        print(f"string at 0x{idx:x}")
        for pc, kind in find_refs(ws, idx):
            print(f"  ref 0x{pc:x} ({kind})  func ~0x{func_start(ws, pc):x}")
    elif cmd == "addr":
        for pc, kind in find_refs(ws, int(arg, 16)):
            print(f"  ref 0x{pc:x} ({kind})  func ~0x{func_start(ws, pc):x}")
    elif cmd == "calls":
        for pc in find_calls(ws, int(arg, 16)):
            print(f"  call 0x{pc:x}  func ~0x{func_start(ws, pc):x}")
    elif cmd == "func":
        print(hex(func_start(ws, int(arg, 16))))


if __name__ == "__main__":
    main()
