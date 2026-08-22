"""Translate the U-Boot PCIe combo-PHY init into an EDK2 op table.

Deliberately strict: every statement inside the function must match one of the
recognised forms, or this raises. A silently dropped register write would be a
very hard bug to find on hardware, so the parser refuses to guess.
"""
import io, re, sys

SRC = "/tmp/ubref/sunxi-cadence-combophy.c"
FIRST, LAST = 99, 258          # 1-based, inclusive: the function body

lines = io.open(SRC, encoding="utf-8", errors="replace").read().split("\n")
body = lines[FIRST:LAST - 1]   # skip signature line and closing brace

# strip comments and blanks
clean = []
for ln in body:
    s = ln.strip()
    s = re.sub(r"/\*.*?\*/", "", s).strip()
    s = re.sub(r"//.*$", "", s).strip()
    if s:
        clean.append(s)

TOP, PHY = "A733_PHY_TGT_TOP", "A733_PHY_TGT_PHY"

def target(expr):
    expr = expr.strip()
    m = re.match(r"^combo1->(top_reg|phy_reg)(?:\s*\+\s*(0x[0-9a-fA-F]+|\d+))?$", expr)
    if not m:
        return None
    tgt = TOP if m.group(1) == "top_reg" else PHY
    off = int(m.group(2), 0) if m.group(2) else 0
    return tgt, off

ops = []
i = 0
unmatched = []

while i < len(clean):
    s = clean[i]

    # writew(VAL, ADDR);  /  writel(VAL, ADDR);
    m = re.match(r"^(writew|writel)\(\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(.+?)\s*\);$", s)
    if m:
        t = target(m.group(3))
        if t is None:
            unmatched.append((i, s)); i += 1; continue
        width = 16 if m.group(1) == "writew" else 32
        ops.append(("WRITE%d" % width, t[0], t[1], int(m.group(2), 0), 0))
        i += 1
        continue

    # val = readX(ADDR);  then a run of val ops, then writeX(val, ADDR);
    m = re.match(r"^val\s*=\s*(readl|readw)\((.+?)\);$", s)
    if m:
        t = target(m.group(2))
        if t is None:
            unmatched.append((i, s)); i += 1; continue
        width = 32 if m.group(1) == "readl" else 16
        full = (1 << width) - 1
        and_mask, or_mask, forced = full, 0, None
        j = i + 1
        while j < len(clean):
            t2 = clean[j]
            mm = re.match(r"^val\s*&=\s*~\(?(0x[0-9a-fA-F]+|\d+|BIT\(\d+\))\)?;$", t2)
            if mm:
                v = mm.group(1)
                v = (1 << int(v[4:-1])) if v.startswith("BIT(") else int(v, 0)
                and_mask &= (~v) & full
                j += 1
                continue
            mm = re.match(r"^val\s*\|=\s*\(?(0x[0-9a-fA-F]+|\d+|BIT\(\d+\))\)?;$", t2)
            if mm:
                v = mm.group(1)
                v = (1 << int(v[4:-1])) if v.startswith("BIT(") else int(v, 0)
                or_mask |= v
                j += 1
                continue
            mm = re.match(r"^val\s*=\s*(0x[0-9a-fA-F]+|\d+);$", t2)
            if mm:
                forced = int(mm.group(1), 0)
                j += 1
                continue
            break

        mm = re.match(r"^(writel|writew)\(\s*val\s*(?:\|\s*(0x[0-9a-fA-F]+|\d+))?\s*,\s*(.+?)\s*\);$", clean[j] if j < len(clean) else "")
        if not mm:
            # a read whose value is discarded, immediately before the poll loop
            nxt = clean[j] if j < len(clean) else ""
            if nxt.startswith("while (true)") or nxt.startswith("while(true)"):
                i = j
                continue
            unmatched.append((i, s + "   [no matching write at %d]" % j)); i += 1; continue
        t3 = target(mm.group(3))
        if t3 != t:
            unmatched.append((i, s + "   [read/write target mismatch]")); i += 1; continue
        if mm.group(2):
            or_mask |= int(mm.group(2), 0)

        if forced is not None:
            ops.append(("WRITE%d" % width, t[0], t[1], forced, 0))
        else:
            ops.append(("ANDOR%d" % width, t[0], t[1], or_mask, and_mask))
        i = j + 1
        continue

    # the PMA-ready poll
    if s.startswith("while (true)") or s.startswith("while(true)"):
        blk = "\n".join(clean[i:i + 8])
        mm = re.search(r"readl\(combo1->top_reg \+ (0x[0-9a-fA-F]+)\)", blk)
        mb = re.search(r"val & BIT\((\d+)\)", blk)
        if not (mm and mb):
            unmatched.append((i, "unrecognised poll loop")); i += 1; continue
        ops.append(("POLL32", TOP, int(mm.group(1), 0), 1 << int(mb.group(1)), 0))
        while i < len(clean) and not clean[i].startswith("}"):
            i += 1
        i += 1
        continue

    # benign scaffolding
    if re.match(r"^(struct |u32 val;|val = readl\(combo1->top_reg\);|udelay\(\d+\);|break;|if \(val & BIT\(\d+\)\)|\}|\{)", s):
        i += 1
        continue

    unmatched.append((i, s))
    i += 1

if unmatched:
    print("REFUSING TO EMIT - %d unrecognised statements:" % len(unmatched))
    for n, s in unmatched:
        print("  line %d: %s" % (n, s))
    sys.exit(1)

# sanity: count raw writes in the source vs ops emitted
raw_writes = sum(1 for s in clean if re.match(r"^(writew|writel)\(\s*0x", s))
print("// source had %d direct writes; emitted %d ops total" % (raw_writes, len(ops)))

out = []
out.append("STATIC CONST A733_PHY_OP  mPciePhyInitOps[] = {")
for op, tgt, off, val, mask in ops:
    if op.startswith("ANDOR"):
        out.append("  { %-18s, %-22s, 0x%05X, 0x%08X, 0x%08X },"
                   % (tgt, "A733_PHY_OP_" + op, off, val, mask))
    elif op == "POLL32":
        out.append("  { %-18s, %-22s, 0x%05X, 0x%08X, 0x%08X },"
                   % (tgt, "A733_PHY_OP_" + op, off, val, 0))
    else:
        out.append("  { %-18s, %-22s, 0x%05X, 0x%08X, 0x%08X },"
                   % (tgt, "A733_PHY_OP_" + op, off, val, 0))
out.append("};")

io.open("/tmp/phyops.inc", "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print("\n".join(out[:6]))
print("...")
print("\n".join(out[-4:]))
print("\ntotal ops: %d  -> /tmp/phyops.inc" % len(ops))
