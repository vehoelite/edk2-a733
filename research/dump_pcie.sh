#!/bin/bash
python3 - <<'PY'
import mmap, os, struct
def dump(base, size, label):
    pagesize=4096
    aligned=base & ~(pagesize-1); off=base-aligned
    length=((off+size+pagesize-1)//pagesize)*pagesize
    fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
    try: m=mmap.mmap(fd,length,mmap.MAP_SHARED,mmap.PROT_READ,offset=aligned)
    except Exception as e: print(f"FAIL {label}: {e}"); os.close(fd); return
    print(f"\n==== {label} @ 0x{base:08x} size 0x{size:x} ====")
    for i in range(0,size,16):
        line=m[off+i:off+i+16]
        if any(b for b in line):
            w=struct.unpack("<IIII",line)
            print(f"  +0x{i:04x}: {w[0]:08x} {w[1]:08x} {w[2]:08x} {w[3]:08x}")
    m.close(); os.close(fd)

# DesignWare PCIe DBI
dump(0x06000000, 0x1000, "DBI base (port logic)")
dump(0x06000700, 0x200, "DBI ATU/iATU area1")
dump(0x06000c00, 0x200, "DBI ATU/iATU area2 (newer)")
dump(0x06300000, 0x100, "DBI+0x300000 (iATU classic)")
# config space root port
dump(0x22200000, 0x1000, "RP cfg (BUS0:DEV0:FN0)")
# downstream NVMe
dump(0x22100000, 0x100, "NVMe BAR0 head")
PY
