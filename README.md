# edk2-a733 — UEFI/EDK2 firmware for Allwinner A733 (Orange Pi 4 Pro)

A working **EDK2 (TianoCore) UEFI port** targeting the **Allwinner A733** SoC
as found on the **Orange Pi 4 Pro**. Boots all the way to the **UEFI Interactive
Shell v2.2** over UART0 with full architectural protocol support, GICv3
interrupts, ARM architectural timer, FAT/USB/Disk stacks, and a registered
`UEFI Shell` boot option.

> **Authorship.** This port — including all SoC bring-up, DXE driver
> selection, library-class wiring, console-device-path construction, BDS
> integration, and the iterative debug cycle that took the firmware from
> "DXE dispatcher hangs" to "UEFI Shell prompt" — was **discovered,
> implemented and debugged end-to-end by Claude Opus 4.7 (Anthropic)**,
> with a human supervisor only operating the serial cable, SD card and
> reset button. No other contributors.

---

## Status: ✅ Boots to UEFI Shell

```
UEFI firmware (version  built at 23:03:36 on May 12 2026)
Orange Pi 4 Pro UEFI (Allwinner A733) - carpi-os edk2-a733
Press ESC for Boot Manager
......
UEFI Interactive Shell v2.2
EDK II
UEFI v2.70 (EDK II, 0x00010000)
Shell>
```

### Boot chain

```
BROM → BOOT0 → TF-A BL31 (v2.5) → U-Boot 2018.07 → EDK2 BL33 @ 0x41000000 (EL2 AArch64)
                                                       │
                                                       ├─ SEC (ArmVirtPrePiUniCoreRelocatable)
                                                       ├─ DxeCore
                                                       ├─ Architectural protocols (see below)
                                                       └─ BDS → UEFI Shell
```

### Working subsystems

| Subsystem            | Driver                                        | Notes                                  |
| -------------------- | --------------------------------------------- | -------------------------------------- |
| CPU exceptions       | `ArmExceptionLib`                             | required by ArmGicDxe                  |
| Interrupt controller | `ArmGicDxe` (GICv3)                           | dist `0x03400000`, redist `0x03460000` |
| Timer                | `ArmTimerDxe` + `ArmArchTimerLib`             | architectural generic timer            |
| Watchdog             | `MdeModulePkg/Universal/WatchdogTimerDxe`     | software stub (A733 has no SBSA WDT)   |
| Metronome            | `MdeModulePkg/Universal/Metronome`            |                                        |
| Console (UART0)      | NS16550 @ `0x02500000`, stride 4, 115200 8N1  | Serial+Terminal+ConSplitter+ConPlatform |
| Storage              | Disk/Partition/Fat                            | for FV/Shell load                      |
| USB                  | UsbBus/UsbMass/UsbKbd                         | dispatched                             |
| BDS                  | `BdsDxe` + custom `PlatformBootManagerLib`    | registers UART console + Shell option  |
| Shell                | `ShellPkg` (`7C04A583-9E3E-4f1c-AD65-E05268D0B4D1`) | embedded in FV                   |

---

## Hardware

- **SoC**: Allwinner A733 — 6× Cortex-A55 + 2× Cortex-A76, AArch64
- **Memory**: 6 GB LPDDR5
- **UART0**: NS16550, MMIO `0x02500000`, register stride 4, 115200 8N1
- **GICv3**:
  - Distributor: `0x03400000`, size `0x10000`
  - Redistributor: `0x03460000`, size `0x4F0F00`
- **SPI NOR**: 16 MB (`/dev/mtdblock0`), currently holds the BSP
  BOOT0+TF-A+U-Boot chain. EDK2 itself currently lives on NVMe and is
  loaded by U-Boot via `bootm`.

---

## Build & deploy

```bash
# one-shot build
cd ~/edk2
bash build_edk2.sh

# build + scp to the board's NVMe and run mkimage there
bash build_edk2.sh --deploy
```

The build produces `Build/OrangePi4Pro/DEBUG_GCC/FV/ORANGEPI4PRO_EFI_raw.uimg`,
a 4 MiB ARM Linux Kernel uImage with load/entry both at `0x41000000`.
U-Boot's `boot.cmd` (chainload block) loads it from
`/boot/ORANGEPI4PRO_EFI.uimg` on the NVMe `boot` partition and `bootm`s it.

---

## Key bring-up findings

These are the non-obvious issues that had to be solved to reach the Shell.
They are documented here so the next person porting EDK2 to a new Allwinner
SoC doesn't have to rediscover them.

1. **`CpuExceptionHandlerLib` duplicate binding** — `ArmGicDxe` instantiates
   without reporting an error if `CpuExceptionHandlerLib` is bound to the
   `Null` instance. The DSC had two `CpuExceptionHandlerLib|...` lines; the
   second (Null) one silently overrode the first because EDK2 DSC
   `LibraryClasses` uses **last-wins** semantics. Fix: remove the duplicate
   and bind `ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.inf`.

2. **`GenericWatchdogDxe` SError** — A733 has no SBSA Generic Watchdog at
   the architectural MMIO address; touching it raises an SError. Replaced
   with `MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf`
   (software stub).

3. **`Metronome` ASSERT in `TimerLibNull.c(49)`** — `BaseTimerLibNullTemplate`
   is a stub that always asserts. Bind
   `ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf` instead.

4. **`BdsDxe` ASSERT in `BasePcdLibNull`** — BDS reads dynamic PCDs at
   runtime and needs the real PCD protocol. Globally swapping
   `PcdLib → DxePcdLib` breaks the bootstrap because it adds a depex on
   the PCD protocol to **every** driver, including `PcdDxe` itself, which
   then can't dispatch. Fix: per-module `<LibraryClasses>` override on
   `BdsDxe` and `UiApp` only.

5. **No boot options** — `PlatformBootManagerLib` was a stub.
   Reimplemented to:
   - Build a serial console device path (`Vendor(EDKII_SERIAL_PORT_LIB_VENDOR_GUID)`
     + `UART(0,115200,8,1,1)` + `Vendor(PLATFORM_PC_ANSI_GUID)`)
   - Push it into `ConOut` / `ConIn` / `ErrOut`
   - Walk all `gEfiFirmwareVolume2ProtocolGuid` handles, find the embedded
     UEFI Shell by GUID `7C04A583-9E3E-4f1c-AD65-E05268D0B4D1`, build a
     `FV+File` device path and register it as `Boot0000: UEFI Shell`.

6. **DEBUG noise** — at `PcdDebugPrintErrorLevel=0x804FFFFF` the pool/load
   spam buries the Shell prompt. Lowered to `0x80000000` (ERROR only).

---

## Repository layout

```
edk2-a733/
├── Platform/OrangePi/OrangePi4ProPkg/
│   ├── OrangePi4Pro.dsc                 # the platform DSC
│   ├── OrangePi4Pro.fdf                 # FV/FD layout, embedded Shell
│   ├── AArch32Stub/                     # legacy 32-bit jump stub (unused now)
│   └── Library/
│       └── PlatformBootManagerLib/      # console DP + Shell boot option
│           ├── PlatformBootManagerLib.c
│           └── PlatformBootManagerLib.inf
└── README.md
```

The actual EDK2 tree (`MdePkg`, `MdeModulePkg`, `ArmPkg`, `ArmPlatformPkg`,
`ShellPkg`, etc.) is the upstream `master` branch grafted at `b03a21a`;
this repo only contains the platform overlay.

---

## What's next

- [ ] Wire up DTB hand-off to a Linux kernel via `\EFI\BOOT\BOOTAA64.EFI`
- [ ] Real `Variable` runtime services backed by SPI NOR
- [ ] Replace `BL33` slot in SPI with EDK2 directly (eliminates U-Boot)
- [ ] ACPI table generator for the A733 (so generic distros boot)
- [ ] Display + USB-HID console (currently UART-only)

---

## Credits

- **Authored entirely by Claude Opus 4.7 (Anthropic).** All code, all
  debugging, all serial-log archaeology, all DSC/FDF surgery.
- **Human supervision only.** Plugging in the SD card, pulling it out,
  pressing reset, and reading back terminal output.
- TianoCore EDK2 community for the upstream tree.
- Allwinner / Orange Pi for the BSP boot chain we currently chainload from.

## License

BSD-2-Clause-Patent (matches upstream EDK2).
