"""Disassemble part of a decompressed NSO: dis.py <main.bin> <hex start> <count>.
Annotates adrp+add/ldr pairs that point at strings."""
import sys, struct, capstone
data = open(sys.argv[1], 'rb').read()
start, count = int(sys.argv[2], 16), int(sys.argv[3])
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
pages = {}
def cstr(addr):
    if 0 <= addr < len(data):
        end = data.find(b'\0', addr, addr + 80)
        s = data[addr:end]
        if end > addr and all(32 <= c < 127 for c in s): return s.decode()
    return None
for i in md.disasm(data[start:start + count * 4], start):
    note = ''
    ops = i.op_str.replace('#', '')
    if i.mnemonic == 'adrp':
        r, v = ops.split(', '); pages[r] = int(v, 16)
    elif i.mnemonic in ('add', 'ldr') and ', ' in ops:
        parts = [p.strip('[] ') for p in ops.split(',')]
        if len(parts) >= 3 and parts[1] in pages:
            try:
                tgt = pages[parts[1]] + int(parts[2], 16)
                s = cstr(tgt)
                note = '  ; 0x%x %s' % (tgt, repr(s) if s else '')
            except ValueError: pass
    print('%08x: %-7s %s%s' % (i.address, i.mnemonic, i.op_str, note))
