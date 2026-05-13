#!/bin/bash
# Dump CCU + USB PHY register state via devmem2 or python
which devmem2 || apt-get install -y devmem2 2>/dev/null || true
which devmem || true
# Use python /dev/mem dumper since devmem may not be present
python3 - <<'PY'
import mmap, os, struct
def dump(base, size, label):
    pagesize = 4096
    aligned = base & ~(pagesize-1)
    off = base - aligned
    length = ((off + size + pagesize - 1) // pagesize) * pagesize
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    try:
        m = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ, offset=aligned)
    except Exception as e:
        print(f"FAIL {label} @0x{base:x}: {e}")
        os.close(fd); return
    print(f"\n==== {label} @ 0x{base:08x} size 0x{size:x} ====")
    for i in range(0, size, 16):
        line = m[off+i:off+i+16]
        if any(b for b in line):
            words = struct.unpack("<IIII", line)
            print(f"  +0x{i:04x}: {words[0]:08x} {words[1]:08x} {words[2]:08x} {words[3]:08x}")
    m.close(); os.close(fd)

dump(0x02002000, 0x2000, "CCU")
dump(0x07010000, 0x340, "R_CCU")
dump(0x06b00000, 0x800, "USB2_PHY")
dump(0x04101000, 0x1000, "EHCI0/OHCI0")
dump(0x04200000, 0x1000, "EHCI1/OHCI1")
dump(0x06a00000, 0x100, "XHCI2_CAP")
dump(0x06c00000, 0x400, "SERDES_LO")
dump(0x06c06000, 0x2000, "SERDES_HI")
PY
