# edk2-a733 — UEFI/EDK2 port for Allwinner A733 (Orange Pi 4 Pro)

A work-in-progress EDK2 (TianoCore) port targeting the **Allwinner A733** SoC
as found on the **Orange Pi 4 Pro**. Boots through the stock BSP boot chain
(BROM → BOOT0 → TF-A BL31 → U-Boot → EDK2 BL33).

## Status

✅ **Working**
- TF-A BL31 jumps cleanly to BL33 (EDK2) at `0x41000000` in AArch64 EL2 (`spsr=0x3c5`)
- ARM64 Linux-style image header trampoline at FD offset 0 (loaded via U-Boot
  `bootm` with an ARM Linux Kernel uImage)
- `_ModuleEntryPoint` (SEC) reached, FP/SIMD enabled
- DTB pointer received via `x0`, magic-checked, packed and copied to slack space
- PE/COFF self-relocation
- 6 GB DRAM + 1 GB device MMU map, MMU enabled successfully
- HOB list construction, library constructors, `MemoryPeim`
- `DxeCore` loaded and dispatched, FV2 extracted
- Architectural protocol slots discovered

❌ **Currently blocked**
- DXE dispatcher cannot resolve any architectural protocol drivers
  (`CpuDxe`, `TimerDxe`, `BdsDxe`, ...). DxeCore asserts in
  `DxeMain.c(578)` with `Status = Not Found` because none of the arch
  protocol producers were dispatched. Likely a DepEx / library / PCD
  mismatch in the FVMAIN driver list. Investigation in progress.

## Hardware

| Component       | Detail                                       |
|-----------------|----------------------------------------------|
| SoC             | Allwinner A733 (sun60iw2)                   |
| CPU             | 6× Cortex-A55 + 2× Cortex-A76 DynamIQ       |
| RAM             | 6 GB LPDDR5                                  |
| Board           | Orange Pi 4 Pro                              |
| Debug UART      | UART0 @ `0x02500000` (NS16550, 32-bit stride)|
| TF-A            | v2.5 (ships in board BSP, AArch32→AArch64)  |
| Boot loader     | U-Boot 2018.07 (AArch32) — uses `bootm`      |
| BL33 load addr  | `0x41000000`                                 |

## Memory layout (FD)

```
0x00000000  +-----------------------------+
            | ARM64 Linux header trampoline|
            |   adr x1, .                  |  ← x1 = FD runtime base
            |   b   _ModuleEntryPoint      |  ← patched post-build
            |   text_offset, image_size,   |
            |   ... "ARMd" magic @ +0x38   |
            +-----------------------------+
0x00000040  | (slack — DTB copy lands here)|
            +-----------------------------+
0x00040000  | FVMAIN_COMPACT (FV)          |  ← PcdFvBaseAddress
            |   PrePi (SEC, relocatable)   |
            |   FVMAIN (compressed):       |
            |     DxeCore + arch drivers   |
            +-----------------------------+
0x002C0000  | NV_VARIABLE_STORE            |
0x00300000  | NV_FTW_WORKING / NV_FTW_SPARE|
0x00400000  +-----------------------------+
```

The 256 KB header is required because the DTB delivered by U-Boot is
~215 KB and `CopyFdt()` lands it in the slack between the trampoline
header and FVMAIN_COMPACT.

## Repository layout

```
Platform/OrangePi/OrangePi4ProPkg/   — DSC/FDF + ArmPlatformLib
Silicon/Allwinner/A733Pkg/           — A733 UART (NS16550 wrapper)
patches/                             — Debug instrumentation patches
                                       to apply on top of EDK2
build_edk2.sh                        — Build, post-link branch patch,
                                       wrap as ARM Linux uImage,
                                       optional NVMe deploy
```

## Build

```bash
# 1. Clone EDK2 next to this repo and bootstrap
git clone https://github.com/tianocore/edk2.git ~/edk2
cd ~/edk2 && git submodule update --init

# 2. Drop platform package into EDK2 tree
cp -r /path/to/edk2-a733/Platform   .
cp -r /path/to/edk2-a733/Silicon    .

# 3. (Optional, current debug build) apply debug-uart patches
git apply /path/to/edk2-a733/patches/0001-prepi-debug-instrumentation.patch

# 4. Toolchain: aarch64-linux-gnu-gcc
sudo apt install gcc-aarch64-linux-gnu uuid-dev nasm acpica-tools \
                 build-essential u-boot-tools

# 5. Build
cp /path/to/edk2-a733/build_edk2.sh .
bash build_edk2.sh
```

The script:
1. Runs `build -p Platform/OrangePi/OrangePi4ProPkg/OrangePi4Pro.dsc -a AARCH64 -t GCC -b DEBUG`
2. Patches the `b <entry>` placeholder in the trampoline header with the
   correct branch encoding to `_ModuleEntryPoint` (located by scanning for
   the SEC's first instruction signature)
3. Wraps the patched 4 MB FD in an ARM Linux Kernel uImage at load
   address `0x41000000` so U-Boot `bootm` can launch it.

## Boot pipeline

```
BROM (mask)
  → BOOT0 (SPI/MMC, init DRAM)
  → TF-A BL31 (AArch64 monitor) ── PSCI / GICv3 / SCP startup
  → U-Boot 2018.07 (AArch32)
    │  bootm 0x41000000 - 0x43000000
    │   (kernel uImage = our EDK2 FD)
    │   (FDT blob          = U-Boot's runtime DTB)
    ↓
  TF-A bootflow back to AArch64 EL2, jumps 0x41000000
    → trampoline → _ModuleEntryPoint → PrePi/SEC → DxeCore → ...
```

## Debug technique

We added incremental UART writes to `0x02500000` ("EVF1234abczgdf45..." +
text via `SerialPortWrite`) so the live picocom log pinpoints the last
function that returned. This is what unblocked the staged bring-up:

| Marker(s)                     | Meaning                                  |
|-------------------------------|------------------------------------------|
| `E` `V` `F`                   | Entry / pre-VFP / post-VFP               |
| `1` `2`                       | DiscoverDramFromDt entered, magic ok      |
| `3` `4`                       | RelocatePeCoffImage call/return           |
| `a..f`                        | C-level steps inside RelocatePeCoffImage  |
| `5`                           | FindMemnode returned                      |
| `pqr<size>:s`                 | CopyFdt: pack / size / copy / done        |
| `D`                           | DiscoverDramFromDt returned               |
| `S`                           | Stack setup, jumping into CEntryPoint     |
| `MMU1` / `MMU2`               | ArmConfigureMmu enter / return            |
| `EDK2: ...` lines             | Real SerialPortWrite, post-MMU            |

## Next steps

1. Diagnose why FVMAIN architectural drivers are not being dispatched
2. Once arch protocols come up, wire BdsDxe to a console/storage path
3. Wire HDMI / GOP via real Allwinner display IP (long-term)
4. NVRAM persistence (currently NV regions are placed in RAM, not SPI)
5. ACPI / SMBIOS table publication

## License

BSD-2-Clause-Patent (matches upstream EDK2).

## Credits

Based on `ArmVirtPkg/PrePi`. Thanks to the upstream EDK2 / TF-A / U-Boot
projects, and the Sunxi / linux-sunxi community for documentation of the
A-series SoCs.
