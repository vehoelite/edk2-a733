import mmap, os, struct, time, sys

REGIONS = [("CCU", 0x02002000, 0x2000), ("R_CCU", 0x07010000, 0x400)]
DRV = "/sys/bus/platform/drivers/sunxi-pcie"
DEV = "6000000.pcie"

def snap():
    out = {}
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        for name, base, size in REGIONS:
            m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
            data = m.read(size); m.close()
            for off in range(0, size, 4):
                out[(name, base + off)] = struct.unpack_from("<I", data, off)[0]
    finally:
        os.close(fd)
    return out

def bound():
    return os.path.exists(os.path.join(DRV, DEV))

def write(path, val):
    with open(path, "w") as f:
        f.write(val)

print("driver bound at start:", bound())
before = snap()
print("snapshot A taken (PCIe UP):", len(before), "registers")

try:
    write(os.path.join(DRV, "unbind"), DEV)
    time.sleep(1.5)
    print("driver bound after unbind:", bound())
    after = snap()
    print("snapshot B taken (PCIe DOWN):", len(after), "registers")
finally:
    if not bound():
        try:
            write(os.path.join(DRV, "bind"), DEV)
            time.sleep(1.5)
        except OSError as e:
            print("REBIND FAILED:", e)
    print("driver bound at end:", bound())

print("\n=== registers that changed (UP -> DOWN) ===")
n = 0
for k in sorted(before):
    if before[k] != after.get(k):
        name, addr = k
        a, b = before[k], after[k]
        print(f"  {name:6s} 0x{addr:08X}  0x{a:08X} -> 0x{b:08X}   (delta bits 0x{a ^ b:08X})")
        n += 1
print(f"total changed: {n}")
