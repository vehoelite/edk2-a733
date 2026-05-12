#!/usr/bin/env bash
# Build EDK2 for OrangePi 4 Pro, patch the entry-point branch, package and deploy.
# Usage: ./build_edk2.sh [--deploy]
set -eo pipefail   # NOTE: no -u — edksetup.sh references unset vars

BOARD_IP="192.168.0.244"
BOARD_USER="orangepi"
BOARD_PASS="orangepi"
FD="Build/OrangePi4Pro/DEBUG_GCC/FV/ORANGEPI4PRO_EFI.fd"
UIMG="Build/OrangePi4Pro/DEBUG_GCC/FV/ORANGEPI4PRO_EFI_arm32.uimg"
FD_BASE=0x41000000

# ── 1. Build ──────────────────────────────────────────────────────────────────
cd "$(dirname "$0")"
export GCC_AARCH64_PREFIX=aarch64-linux-gnu-
. edksetup.sh BaseTools
build -p Platform/OrangePi/OrangePi4ProPkg/OrangePi4Pro.dsc -a AARCH64 -t GCC -b DEBUG

# ── 2. Find _ModuleEntryPoint and patch the branch at FD offset 4 ─────────────
python3 - <<'PYEOF'
import struct, sys

FD_PATH   = "Build/OrangePi4Pro/DEBUG_GCC/FV/ORANGEPI4PRO_EFI.fd"
FD_BASE   = 0x41000000
BRANCH_PC = FD_BASE + 4   # runtime address of the 'b' instruction

# _ModuleEntryPoint now starts with our debug prologue:
#   movz x10, #0x0250    (encoding 0xd2804a0a)
#   lsl  x10, x10, #16   (encoding 0xd370bd4a)
# These are unique enough to locate the entry point reliably.
NEEDLE = struct.pack('<II', 0xd2804a0a, 0xd370bd4a)

with open(FD_PATH, "rb") as f:
    fd = bytearray(f.read())

# Search from 0x40000 onward (entry is inside FVMAIN_COMPACT, not the 256KB header)
idx = fd.find(NEEDLE, 0x40000)
if idx < 0:
    sys.exit("ERROR: _ModuleEntryPoint signature (cbnz x0, #8) not found in FD!")

entry_fd_offset = idx
entry_runtime   = FD_BASE + entry_fd_offset
branch_offset   = entry_runtime - BRANCH_PC  # bytes, must be divisible by 4
assert branch_offset % 4 == 0, f"branch offset {branch_offset:#x} not 4-byte aligned"
imm26 = (branch_offset >> 2) & 0x3FFFFFF
b_enc = struct.pack('<I', 0x14000000 | imm26)

print(f"_ModuleEntryPoint FD offset : {entry_fd_offset:#x}")
print(f"_ModuleEntryPoint runtime   : {entry_runtime:#010x}")
print(f"Branch encoding             : {0x14000000 | imm26:#010x} -> bytes {b_enc.hex()}")

# Verify current placeholder (b #0 = 0x14000000)
cur = struct.unpack_from('<I', fd, 4)[0]
if cur != 0x14000000:
    print(f"WARNING: Expected placeholder b #0 (0x14000000) at offset 4, got {cur:#010x}")

fd[4:8] = b_enc

with open(FD_PATH, "wb") as f:
    f.write(fd)

print("Branch patched successfully.")
PYEOF

# ── 3. Package as uImage ──────────────────────────────────────────────────────
mkimage -A arm -O linux -T kernel -C none \
    -a 0x41000000 -e 0x41000000 \
    -n "EDK2 UEFI" \
    -d "$FD" "$UIMG"
echo "uImage built: $UIMG"

# ── 4. Deploy (optional) ──────────────────────────────────────────────────────
if [[ "${1:-}" == "--deploy" ]]; then
    echo "Deploying to $BOARD_IP NVMe boot partition..."
    sshpass -p "$BOARD_PASS" scp -o StrictHostKeyChecking=no \
        "$UIMG" "${BOARD_USER}@${BOARD_IP}:/tmp/ORANGEPI4PRO_EFI.uimg"
    sshpass -p "$BOARD_PASS" ssh -o StrictHostKeyChecking=no \
        "${BOARD_USER}@${BOARD_IP}" \
        "echo $BOARD_PASS | sudo -S bash -c '
          mountpoint -q /mnt/nvme || { mkdir -p /mnt/nvme && mount /dev/nvme0n1p1 /mnt/nvme; }
          cp /tmp/ORANGEPI4PRO_EFI.uimg /mnt/nvme/boot/ORANGEPI4PRO_EFI.uimg
          sync
          # Also update SD card /boot in case board boots from SD
          cp /tmp/ORANGEPI4PRO_EFI.uimg /boot/ORANGEPI4PRO_EFI.uimg 2>/dev/null || true
          sync
          echo \"Deployed to NVMe: \$(md5sum /mnt/nvme/boot/ORANGEPI4PRO_EFI.uimg)\"
        '"
fi
