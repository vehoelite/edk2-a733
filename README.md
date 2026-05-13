# edk2-a733 — UEFI/EDK2 firmware for Allwinner A733 (Orange Pi 4 Pro)

A working **EDK2 (TianoCore) UEFI port** targeting the **Allwinner A733** SoC
as found on the **Orange Pi 4 Pro**. Boots all the way to a **graphical UEFI
Boot Manager and Interactive Shell rendered natively on the panel**, with a
serial fallback over UART0 — using a from-scratch GOP driver that takes
ownership of the DE3.0 mixer0 scanout pipeline.

> **Authorship.** This entire port — SoC bring-up, DXE driver
> selection, library-class wiring, console-device-path construction, BDS
> integration, the from-scratch DE3.0 mixer0 framebuffer takeover, GOP +
> Software Blt + ConSplitter wiring, and every line of the iterative
> debug cycle that took the firmware from "DXE dispatcher hangs" to
> "EDK2 Boot Manager rendering on the LCD" — was **discovered,
> implemented and debugged end-to-end by Claude Opus 4.7 (Anthropic)**,
> with a human supervisor only operating the serial cable, SD card and
> reset button. No other contributors.

---

## Status

| Subsystem               | State | Notes                                                          |
| ----------------------- | ----- | -------------------------------------------------------------- |
| Boot to UEFI Shell      | ✅    | UART + native panel                                            |
| Native panel display    | ✅    | DE3.0 mixer0 scanout takeover; 1024×600 BGRA8888               |
| GOP + GraphicsConsole   | ✅    | EDK2 Boot Manager + Shell prompt render on the LCD             |
| GICv3 + ArchTimer       | ✅    | full architectural protocols                                   |
| FAT / Partition / Disk  | ✅    | dispatched, ready for storage backends                         |
| USB host stack          | ⚠️    | drivers loaded; controllers blocked (see § Closed BSP walls)   |
| PCIe / NVMe             | ⚠️    | link is up; config-space DBI is access-locked (see § same)     |
| Variable runtime        | ❌    | no SPI NOR variable backend yet                                |
| ACPI                    | ❌    | no DSDT generator yet                                          |

### Visual proof

After EDK2 takes the panel:

- DE3.0 mixer0 layer-0 is reprogrammed to scan out an EDK2-owned
  framebuffer at PA `0xFF800000` (4 KiB-aligned, BGRA8888, pitch 4096).
- The panel is filled opaque black before any text is drawn.
- `GraphicsConsoleDxe` binds to the GOP and the Boot Manager + Shell
  prompt appear on the LCD with a hardware-accelerated-looking software
  Blt path (VideoFill / BufferToVideo / VideoToBlt / VideoToVideo, all
  with cache writeback so DE3.0 reads back the right pixels).

```
Orange Pi 4 Pro UEFI (Allwinner A733) - carpi-os edk2-a733
Press ESC for Boot Manager
......
UEFI Interactive Shell v2.2
Shell>
```

---

## Boot chain

```
BROM → BOOT0 → TF-A BL31 (v2.5) → BSP U-Boot 2018.07 → EDK2 BL33 @ 0x41000000 (EL2 AArch64)
                                                          │
                                                          ├─ SEC (ArmVirtPrePiUniCoreRelocatable)
                                                          ├─ DxeCore
                                                          ├─ Architectural protocols (GIC, Timer, ...)
                                                          ├─ SunxiSimpleFbGopDxe ── reprograms DE3.0 mixer0,
                                                          │                          paints opaque black,
                                                          │                          publishes EFI_GRAPHICS_OUTPUT_PROTOCOL
                                                          ├─ GraphicsConsoleDxe ── binds GOP, becomes ConOut
                                                          └─ BDS → Boot Manager / UEFI Shell on panel + UART
```

---

## Hardware

- **SoC**: Allwinner A733 (`sun60iw2p1`) — 6× Cortex-A55 + 2× Cortex-A76, AArch64
- **Memory**: 6 GB LPDDR5
- **UART0**: NS16550, MMIO `0x02500000`, register stride 4, 115200 8N1
- **GICv3**:
  - Distributor: `0x03400000`, size `0x10000`
  - Redistributor: `0x03460000`, size `0x4F0F00`
- **DE3.0 (Display Engine v3)**:
  - mixer0 layer-0 control: `0x05101000` → `0xFF008003` (BGRA, alpha 0xFF, enable)
  - mixer0 layer-0 size: `0x05101004`
  - mixer0 layer-0 pitch: `0x0510100C`
  - mixer0 layer-0 scanout addr: `0x05101018` ← write FB physical here
  - mixer0 DBUFF apply: `0x05100008`
- **Framebuffer**: PA `0xFF800000`, 1024 × 600, BGRA8888, pitch `4096`
- **SPI NOR**: 16 MB (`/dev/mtdblock0`); BSP BOOT0+TF-A+U-Boot lives here.
  EDK2 itself currently lives on NVMe and is loaded by U-Boot via
  `bootm /boot/ORANGEPI4PRO_EFI.uimg`. A factory backup is checked in
  at [`spi_factory_backup.bin`](./spi_factory_backup.bin) so you can
  always restore the stock chain.

---

## Build & deploy

```bash
# one-shot build
cd ~/edk2
bash build_edk2.sh

# build + scp to the board's NVMe and run mkimage there
bash build_edk2.sh --deploy
```

Output: `Build/OrangePi4Pro/DEBUG_GCC/FV/ORANGEPI4PRO_EFI_raw.uimg`,
a 4 MiB ARM Linux Kernel uImage with load/entry both at `0x41000000`.
BSP U-Boot's `boot.cmd` chainload block loads it from
`/boot/ORANGEPI4PRO_EFI.uimg` on the NVMe `boot` partition and `bootm`s it.

---

## Key bring-up findings

These are the non-obvious issues that had to be solved. Documented here
so the next person porting EDK2 to a new Allwinner SoC doesn't have to
rediscover them.

### Core firmware (Shell era)

1. **`CpuExceptionHandlerLib` duplicate binding** — `ArmGicDxe`
   instantiates without reporting an error if `CpuExceptionHandlerLib`
   is bound to the `Null` instance. The DSC had two
   `CpuExceptionHandlerLib|...` lines; the second (Null) silently
   overrode the first because EDK2 DSC `LibraryClasses` uses
   **last-wins** semantics. Fix: bind
   `ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.inf`.

2. **`GenericWatchdogDxe` SError** — A733 has no SBSA Generic Watchdog
   at the architectural MMIO address; touching it raises an SError.
   Replaced with `MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf`
   (software stub).

3. **`Metronome` ASSERT in `TimerLibNull.c(49)`** — `BaseTimerLibNullTemplate`
   is a stub that always asserts. Bind
   `ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf`.

4. **`BdsDxe` ASSERT in `BasePcdLibNull`** — BDS reads dynamic PCDs at
   runtime and needs the real PCD protocol. Globally swapping
   `PcdLib → DxePcdLib` breaks the bootstrap because it adds a depex on
   the PCD protocol to **every** driver, including `PcdDxe` itself.
   Fix: per-module `<LibraryClasses>` override on `BdsDxe` and `UiApp`
   only.

5. **No boot options** — `PlatformBootManagerLib` was a stub.
   Reimplemented to build a serial console device path, push it into
   `ConOut/ConIn/ErrOut`, walk all `gEfiFirmwareVolume2ProtocolGuid`
   handles and register the embedded UEFI Shell
   (`7C04A583-9E3E-4f1c-AD65-E05268D0B4D1`) as `Boot0000`.

### Display bring-up (panel + GOP era)

6. **DE3.0 mixer0 was the only path that made pixels appear.** TCON,
   panel timing, backlight, MIPI-DSI PHY were all left exactly as
   the BSP U-Boot configured them — **only** the scanout address in
   the mixer0 layer-0 register was reprogrammed to point at an
   EDK2-owned framebuffer, then the DBUFF apply bit at `0x05100008`
   was pulsed to commit. That's it.

7. **Cache writeback is mandatory** — DE3.0 fetches scanout from DRAM
   through the system interconnect, not through the CPU caches. Every
   software Blt operation calls `WriteBackDataCacheRange()` over the
   touched FB region, otherwise text appears delayed by one cache-line
   eviction or never at all.

8. **GOP + GraphicsConsole + ConSplitter wiring** —
   `PlatformBootManagerBeforeConsole` does
   `LocateHandleBuffer(gEfiGraphicsOutputProtocolGuid)` and pushes each
   GOP device path into `ConOut` and `ErrOut` via
   `EfiBootManagerUpdateConsoleVariable`. Without this,
   `GraphicsConsoleDxe` binds but ConSplitter never routes any text to
   it and the panel stays black after the EDK2 banner.

### Console / debug

9. **DEBUG noise** — at `PcdDebugPrintErrorLevel=0x804FFFFF` the
   pool/load spam buries the Shell prompt. Lowered to `0x80000000`
   (ERROR only). Note: this **also masks `DEBUG_INFO`**, so when
   debugging your own driver, raise it back temporarily.

10. **Heredoc + `sshpass + sudo bash -s`** mangles bash escapes. Always
    write helper scripts to `/tmp`, scp them, then
    `ssh sudo bash /tmp/script.sh`. (See [`research/`](./research/).)

---

## Closed-BSP walls — current status

Allwinner A733 is **substantially more locked down than typical sun50i
parts**. Three subsystems we wanted to bring up turned out to be gated
behind BSP-only register sequences. As of v0.1 the **register-level
specs have been located** (see [§ References](#references)) and
clean-room EDK2 reimplementations are now **in progress** — but they
are not yet committed:

### Wall 1 — USB host controllers (EHCI0/1, OHCI0/1, xHCI2)

- Controllers respond to MMIO with valid signatures **only after** the
  BSP CCU `clk-sun60iw2-ccu` driver gates their clocks and de-asserts
  the matching resets, **and** after the proprietary
  `phy-sunxi-plat`/`phy-sun60iw2` USB2 PHY driver runs its
  V2-resistance calibration sequence on the PHY block at `0x06B00000`.
- The CCU is at `0x02002000` (`allwinner,sun60iw2-ccu`) — the source of
  `drivers/clk/sunxi-ng/ccu-sun60iw2.c` is **not in any public
  Allwinner release** as of this writing. Without it, the mapping from
  the per-controller clock IDs in the device tree (`hosc=0xdd`,
  `bus_hci ehci0=0xd8`, `res_dcap=0x10b`, etc.) to CCU register
  offsets and bit positions is unknown.
- xHCI3 SuperSpeed additionally requires the **Cadence Combo PHY**
  serdes block at `0x06C00000` + `0x06C06000`. No public driver.
- VBUS is gated through the **AXP515 PMIC** over I²C7 — would need an
  EDK2 I²C controller driver + AXP515 driver to even power the ports.

We saved a complete CCU + USB PHY + USB controller register snapshot
captured from a running BSP Linux kernel (USB working) so a future
contributor can do *register-state replay* without rediscovery:

- [`research/sun60iw2-ccu-phy-usb-snapshot.txt`](research/sun60iw2-ccu-phy-usb-snapshot.txt)
- [`research/sun60iw2-devicetree-raw.txt`](research/sun60iw2-devicetree-raw.txt)
- [`research/sun60iw2-iomem.txt`](research/sun60iw2-iomem.txt)
- [`research/dump_regs.sh`](research/dump_regs.sh) — the `/dev/mem` capture script

### Wall 2 — PCIe / NVMe

- The PCIe controller is DesignWare. The DBI register window at
  `0x06000000` reads back as **all `0xFF`** from a running BSP
  kernel — meaning DBI access is gated by an Allwinner-specific
  unlock register (likely in CCU or a SYS_CTRL block). Without it, no
  driver can program the iATU or read root-port config space.
- The NVMe BAR0 at `0x22100000` *does* respond (`0x0a013FFF` = valid
  NVMe `CAP`), so the link IS up and BSP U-Boot already enumerated and
  assigned BARs. We just can't *enumerate again* from EDK2 because
  `PciHostBridgeDxe` needs config-space cycles and config space is
  locked.
- Snapshot: [`research/sun60iw2-pcie-dbi-snapshot.txt`](research/sun60iw2-pcie-dbi-snapshot.txt)

### Wall 3 — Ethernet

- Same shape as USB: BSP-only CCU clock IDs, BSP-only PHY init.
  Not investigated further.

**Status update:** as of v0.1 we have the BSP kernel
(`bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.[ch]`,
`bsp/drivers/usb/phy/sunxi-awphy-plat.c`,
`bsp/drivers/pcie/pcie-sunxi-rc.c`,
`bsp/drivers/power/supply/axp515_*`) and the SyterKit Radxa Cubie A7A
port (same A733 SoC) as references. Reimplementation in BSD-licensed
EDK2 style is the next milestone.

---

## References

Sources used as **hardware-behaviour specifications** for clean-room
reimplementation in EDK2 (we read them, did not copy them — code in
this repo is fresh BSD-2-Clause-Patent, written in EDK2 style):

- [orangepi-xunlong/orange-pi-5.15-sun60iw2](https://gitee.com/orangepi-xunlong/orange-pi-5.15-sun60iw2)
  (BSP kernel, GPL-2.0) — `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.[ch]`,
  `bsp/drivers/usb/phy/sunxi-awphy-plat.c`,
  `bsp/drivers/pcie/pcie-sunxi-rc.c`,
  `bsp/drivers/power/supply/axp515_*`,
  `bsp/drivers/power/mfd/axp*`
- [orangepi-xunlong/orangepi-build](https://github.com/orangepi-xunlong/orangepi-build)
  (`external/config/sources/families/sun60iw2.conf`,
  `external/packages/pack-uboot/sun60iw2/bin/dts/u-boot-current.dts`)
- [YuzukiHD/SyterKit](https://github.com/YuzukiHD/SyterKit)
  (bare-metal SPL framework, GPL-2.0+) — Radxa Cubie A7A board port
  (`board/radxa-cubie-a7a/`), `include/drivers/chips/sun60iw2/`
  (CCU + NCAT register definitions extracted from BSP U-Boot),
  `src/drivers/chips/sun60iw2/sys-clk.c`

Live register snapshots from a running BSP Linux kernel are in
[`research/`](./research/) so contributors can verify expected end-state
values without needing the board.

---

## Repository layout

```
edk2-a733/
├── Platform/OrangePi/OrangePi4ProPkg/
│   ├── OrangePi4Pro.dsc                 # platform DSC
│   ├── OrangePi4Pro.fdf                 # FV/FD layout, embedded Shell
│   ├── OrangePi4ProPkg.dec
│   ├── Drivers/
│   │   ├── SunxiSimpleFbGopDxe/         # DE3.0 mixer0 scanout takeover + GOP
│   │   └── SunxiUsbDxe/                 # WIP USB host registration (blocked, see Wall 1)
│   ├── Library/
│   │   └── PlatformBootManagerLib/      # console DP + GOP wiring + Shell boot option
│   ├── Include/
│   └── AArch32Stub/                     # legacy 32-bit jump stub (unused)
├── research/                            # live BSP register captures + scripts
├── patches/
├── build_edk2.sh
├── spi_factory_backup.bin               # 16 MB SPI dump for restore
└── README.md
```

The actual EDK2 tree (`MdePkg`, `MdeModulePkg`, `ArmPkg`,
`ArmPlatformPkg`, `ShellPkg`, etc.) is the upstream `master` branch
grafted at `b03a21a`; this repo only contains the platform overlay.

---

## What's next (community help wanted)

- [ ] **`drivers/clk/sunxi-ng/ccu-sun60iw2.[ch]`** — unblocks USB,
      PCIe re-enumeration, Ethernet
- [ ] EDK2 I²C + AXP515 PMIC driver — unblocks USB VBUS
- [ ] DesignWare PCIe `PciHostBridgeLib` for sun60iw2
- [ ] Real `Variable` runtime services backed by SPI NOR
- [ ] Replace BSP `BL33` slot in SPI with EDK2 directly
      (eliminates U-Boot from the chain)
- [ ] ACPI table generator for the A733 (so generic distros boot)
- [ ] DTB hand-off path to a Linux kernel via
      `\EFI\BOOT\BOOTAA64.EFI`

---

## Credits

- **Authored entirely by Claude Opus 4.7 (Anthropic).** All code, all
  debugging, all serial-log archaeology, all DSC/FDF surgery, all
  register reverse-engineering against running silicon.
- **Human supervision only.** Plugging in the SD card, pulling it out,
  pressing reset, reading back terminal output, and choosing which
  walls to bang our head against next.
- TianoCore EDK2 community for the upstream tree.
- Allwinner / Orange Pi for the BSP boot chain we currently chainload
  from.

## License

BSD-2-Clause-Patent (matches upstream EDK2).
