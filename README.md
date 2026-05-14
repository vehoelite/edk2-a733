# edk2-a733 — UEFI/EDK2 firmware for Allwinner A733 (Orange Pi 4 Pro)

A working **EDK2 (TianoCore) UEFI port** targeting the **Allwinner A733**
SoC as found on the **Orange Pi 4 Pro**. Boots all the way to a
**graphical UEFI Boot Manager and Interactive Shell rendered natively on
the panel**, with a serial fallback over UART0, **a working USB host
stack with hub + mass-storage + keyboard support**, and a from-scratch
GOP driver that takes ownership of the DE3.0 mixer0 scanout pipeline.

> **Authorship.** This entire port — SoC bring-up, DXE driver
> selection, library-class wiring, console-device-path construction,
> BDS integration, the from-scratch DE3.0 mixer0 framebuffer takeover,
> GOP + Software Blt + ConSplitter wiring, the clean-room
> sun60iw2 CCU + USB2 PHY bring-up that drives EHCI, and every line
> of the iterative debug cycle that took the firmware from "DXE
> dispatcher hangs" to "EDK2 Shell on the LCD with a USB keyboard you
> can type on" — was **discovered, implemented and debugged
> end-to-end by Claude Opus 4.7 (Anthropic)**, with a human
> supervisor only operating the serial cable, SD card and reset
> button. No other contributors.

---

## Status

| Subsystem               | State | Notes                                                          |
| ----------------------- | ----- | -------------------------------------------------------------- |
| Boot to UEFI Shell      | ✅    | UART + native panel                                            |
| Native panel display    | ✅    | DE3.0 mixer0 scanout takeover; 1024×600 BGRA8888               |
| GOP + GraphicsConsole   | ✅    | EDK2 Boot Manager + Shell prompt render on the LCD             |
| GICv3 + ArchTimer       | ✅    | full architectural protocols                                   |
| FAT / Partition / Disk  | ✅    | dispatched, ready for storage backends                         |
| **USB-A right pair (EHCI1)** | ✅ | **both top + bottom right USB-A ports working**                |
| **USB Mass Storage**    | ✅    | **enumerates as `BLK0`/`BLK1`/`FS0`/`CDROM` in EFI shell**     |
| **USB HID keyboard**    | ✅    | **typing reaches the EFI shell prompt (wildcard ConIn)**       |
| USB-A left bottom (EHCI0) | ⚠️  | PHY up + registered (UTMI_STAT=0x08, PORTSC=0x3000); jack physical wiring TBD |
| USB-A left top (xHCI 3.0) | ❌  | DWC3 wrapper alive but xHCI MMIO dead — needs Cadence Combo PHY init at `0x06C00000` |
| PCIe / NVMe             | ⚠️    | link is up; config-space DBI is access-locked (see § walls)    |
| Variable runtime        | ❌    | no SPI NOR variable backend yet                                |
| ACPI                    | ❌    | no DSDT generator yet                                          |

### Visual proof — v0.2

After EDK2 takes the panel and brings up EHCI1 + USB2 PHY:

```
Orange Pi 4 Pro UEFI (Allwinner A733) - carpi-os edk2-a733
HCI@0x04200000 +0x824 (UTMI_STAT)=0x00000008    <-- USB2 PHY clock valid
EhcInitHC: pre-PSE  USBCMD=0x00080B01 PORTSC=0x00001800
......
UEFI Interactive Shell v2.2
Shell> map -b
FS0:  Alias(s):CD0a0c0a;BLK1:
      VenHw(.../USB(0x0,0x0)/USB(0x2,0x0)/CDROM(0x0))
BLK0: VenHw(.../USB(0x0,0x0)/USB(0x2,0x0))
Shell> _   <-- type on the USB keyboard, characters appear here
```

(USB hub on EHCI1 → HID boot keyboard + USB mass-storage stick → keystrokes
reach `ConSplitter` via `UsbKbDxe`, FAT volume of the stick is mountable.)

### Visual proof — v0.1 (panel)

- DE3.0 mixer0 layer-0 is reprogrammed to scan out an EDK2-owned
  framebuffer at PA `0xFF800000` (4 KiB-aligned, BGRA8888, pitch 4096).
- The panel is filled opaque black before any text is drawn.
- `GraphicsConsoleDxe` binds to the GOP and the Boot Manager + Shell
  prompt appear on the LCD with a hardware-accelerated-looking software
  Blt path.

---

## Boot chain

```
BROM → BOOT0 → TF-A BL31 (v2.5) → BSP U-Boot 2018.07 → EDK2 BL33 @ 0x41000000 (EL2 AArch64)
                                                          │
                                                          ├─ SEC (ArmVirtPrePiUniCoreRelocatable)
                                                          ├─ DxeCore
                                                          ├─ Architectural protocols (GIC, Timer, …)
                                                          ├─ SunxiSimpleFbGopDxe ── reprograms DE3.0 mixer0,
                                                          │                          paints opaque black,
                                                          │                          publishes EFI_GRAPHICS_OUTPUT_PROTOCOL
                                                          ├─ GraphicsConsoleDxe ── binds GOP, becomes ConOut
                                                          ├─ SunxiUsbDxe ──── CCU init (AHB/MBUS/per-controller),
                                                          │                   USB2 PHY bring-up (SIDDQ → tune → reset),
                                                          │                   registers EHCI0/EHCI1 as NonDiscoverable PCI
                                                          ├─ EhciDxe + UsbBusDxe + UsbMassStorageDxe + UsbKbDxe
                                                          └─ BDS → Boot Manager / UEFI Shell on panel + UART + USB-KB
```

---

## Hardware

- **SoC**: Allwinner A733 (`sun60iw2p1`) — 6× Cortex-A55 + 2× Cortex-A76, AArch64
- **Memory**: 6 GB LPDDR5
- **UART0**: NS16550, MMIO `0x02500000`, register stride 4, 115200 8N1
- **GICv3**:
  - Distributor: `0x03400000`, size `0x10000`
  - Redistributor: `0x03460000`, size `0x4F0F00`
- **CCU**: `0x02002000` (`allwinner,sun60iw2-ccu`)
- **SYSCFG**: `0x03000000`
- **DE3.0 (Display Engine v3)**:
  - mixer0 layer-0 control: `0x05101000` → `0xFF008003` (BGRA, alpha 0xFF, enable)
  - mixer0 layer-0 scanout addr: `0x05101018` ← write FB physical here
  - mixer0 DBUFF apply: `0x05100008`
- **Framebuffer**: PA `0xFF800000`, 1024 × 600, BGRA8888, pitch `4096`
- **USB**:
  - OTG: `0x04100000`
  - EHCI0: `0x04101000` (shares PHY with OTG)
  - EHCI1: `0x04200000` ← **working, USB-2 hub + keyboard + mass storage**
  - USB2 awphy: `0x06B00000`
- **SPI NOR**: 16 MB (`/dev/mtdblock0`); BSP BOOT0 + TF-A + U-Boot lives
  here. EDK2 currently lives on NVMe and is loaded by U-Boot via
  `bootm /boot/ORANGEPI4PRO_EFI.uimg`. A factory backup is checked in at
  [`spi_factory_backup.bin`](./spi_factory_backup.bin) so you can always
  restore the stock chain.

---

## Build & deploy

```bash
# one-shot build
cd ~/edk2
bash build_edk2.sh

# build + scp to the board's NVMe and run mkimage there
bash build_edk2.sh --deploy
```

Output: `Build/OrangePi4Pro/DEBUG_GCC/FV/ORANGEPI4PRO_EFI_arm32.uimg`,
a 4 MiB ARM Linux Kernel uImage with load/entry both at `0x41000000`.
BSP U-Boot's `boot.cmd` chainload block loads it from
`/boot/ORANGEPI4PRO_EFI.uimg` on the NVMe `boot` partition and `bootm`s
it. Note: the eMMC and the NVMe `boot` partitions ship with the **same
filesystem UUID**, so a deploy must update both copies or U-Boot will
sometimes load a stale firmware.

---

## Key bring-up findings

These are the non-obvious issues that had to be solved. Documented here
so the next person porting EDK2 to a new Allwinner SoC doesn't have to
rediscover them.

### USB host bring-up (v0.2 — the wall came down)

Bringing up EHCI on a closed-BSP Allwinner part was the single largest
piece of reverse-engineering in this port. Worked end-to-end by reading
live `/dev/mem` snapshots from a running BSP Linux kernel and matching
them against the BSP `sunxi-hci.c` source. Lessons:

1. **HCI `+0x824` bit 3 is "UTMI clock valid", NOT "device present".**
   It is asserted as soon as the USB2 PHY is fully out of suspend +
   reset, regardless of whether anything is plugged in. The Linux
   baseline reads `0x08` on every controller with no device attached;
   that's how we knew the PHY bring-up — not the device-detect logic —
   was wrong.

2. **PHY bring-up ordering matters and the BSP source order is the
   only correct order.** From `sunxi-hci.c` ~L1280–L1410 the sequence
   for each EHCI is:

   ```
   assert PHY reset (clear CCU bit30 of the per-controller PHY clk reg)
   wait 200 µs
   clear FORCE_SUSPEND (HCI +0x800 / passby register)
   clear SIDDQ          (HCI +0x810 / phy_ctrl)
   program passby + phy_tune (+0x800, +0x818)
   write 0x53 to +0x81C
   deassert PHY reset (set CCU bit30)
   wait 1 ms
   read +0x824 — should be 0x08
   ```

   Earlier builds asserted/deasserted the PHY reset *before* clearing
   `SIDDQ`. UTMI clock never came up and `+0x824` stayed `0x00`.

3. **CCU init for sun60iw2 USB.** Required writes (all in the
   `0x02002000` window):

   | Reg     | Value         | Meaning                        |
   | ------- | ------------- | ------------------------------ |
   | `0x05A4`| msi_lite2 gate| MBUS-like access enable        |
   | `0x05C0`| `0xB10103F8`  | AHB gates incl. USB host       |
   | `0x05E0`| `0xF0050803`  | MBUS gate 0                    |
   | `0x05E4`| `0x00000805`  | MBUS gate 1                    |
   | `0x1A00`| `\|= BIT3`    | res_dcap (USB resource cap)    |
   | `0x1300`| `0xC0000000`  | usb0 clock (incl. PHY bit30)   |
   | `0x1304`| `0x00110011`  | usb0 gate + reset              |
   | `0x1308`| `0xC0000000`  | usb1 clock                     |
   | `0x130C`| `0x00110011`  | usb1 gate + reset              |
   | `0x1340`| `0x80000000`  | USB ref enable                 |
   | `0x1348`| `\|= gate`    | USB ref gate                   |
   | `0x1350`| `0x81000000`  | USB ref divider                |
   | `0x1354`| `0x80000000`  | USB ref source select          |
   | `0x135C`| `\|= BIT16`   | USB host ungate                |

   Plus `SYSCFG +0x160 |= BIT10` (USB tunnel enable) and
   `OTG +0x420 &= ~BIT0` (`USBC_SelectPhyToHci` — gives the OTG PHY
   to EHCI0 instead of OTG).

4. **EFI shell needs an explicit USB-keyboard ConIn entry.** Even with
   `UsbKbDxe` + `UsbBusDxe` + `ConPlatformDxe` + `ConSplitterDxe` all
   built in and bound, **no keystrokes reach the shell** unless
   `PlatformBootManagerBeforeConsole` adds a USB-class wildcard device
   path to `ConIn`:

   ```c
   USB_CLASS_DEVICE_PATH UsbClass = {
     ...
     .VendorId  = 0xFFFF, .ProductId = 0xFFFF,
     .DeviceClass = 0x03,   // HID
     .DeviceSubClass = 0x01, // boot interface
     .DeviceProtocol = 0x01  // keyboard
   };
   EfiBootManagerUpdateConsoleVariable (ConIn, &UsbClass, NULL);
   ```

   `ConPlatformDxe` then expands this wildcard against every
   `SimpleTextInputEx` instance `UsbKbDxe` publishes, so any USB
   keyboard on any port (including behind a hub) becomes a console
   input device.

### Core firmware (Shell era — v0.1)

5. **`CpuExceptionHandlerLib` duplicate binding** — `ArmGicDxe`
   instantiates without reporting an error if `CpuExceptionHandlerLib`
   is bound to the `Null` instance. The DSC had two
   `CpuExceptionHandlerLib|...` lines; the second (Null) silently
   overrode the first because EDK2 DSC `LibraryClasses` uses
   **last-wins** semantics. Fix: bind
   `ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.inf`.

6. **`GenericWatchdogDxe` SError** — A733 has no SBSA Generic Watchdog
   at the architectural MMIO address; touching it raises an SError.
   Replaced with `MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf`
   (software stub).

7. **`Metronome` ASSERT in `TimerLibNull.c(49)`** — `BaseTimerLibNullTemplate`
   is a stub that always asserts. Bind
   `ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf`.

8. **`BdsDxe` ASSERT in `BasePcdLibNull`** — BDS reads dynamic PCDs at
   runtime and needs the real PCD protocol. Globally swapping
   `PcdLib → DxePcdLib` breaks the bootstrap because it adds a depex on
   the PCD protocol to **every** driver, including `PcdDxe` itself.
   Fix: per-module `<LibraryClasses>` override on `BdsDxe` and `UiApp`
   only.

9. **No boot options** — `PlatformBootManagerLib` was a stub.
   Reimplemented to build a serial console device path, push it into
   `ConOut/ConIn/ErrOut`, walk all `gEfiFirmwareVolume2ProtocolGuid`
   handles and register the embedded UEFI Shell
   (`7C04A583-9E3E-4f1c-AD65-E05268D0B4D1`) as `Boot0000`.

### Display bring-up (panel + GOP era — v0.1)

10. **DE3.0 mixer0 was the only path that made pixels appear.** TCON,
    panel timing, backlight, MIPI-DSI PHY were all left exactly as
    the BSP U-Boot configured them — **only** the scanout address in
    the mixer0 layer-0 register was reprogrammed to point at an
    EDK2-owned framebuffer, then the DBUFF apply bit at `0x05100008`
    was pulsed to commit. That's it.

11. **Cache writeback is mandatory** — DE3.0 fetches scanout from DRAM
    through the system interconnect, not through the CPU caches. Every
    software Blt operation calls `WriteBackDataCacheRange()` over the
    touched FB region, otherwise text appears delayed by one cache-line
    eviction or never at all.

12. **GOP + GraphicsConsole + ConSplitter wiring** —
    `PlatformBootManagerBeforeConsole` does
    `LocateHandleBuffer(gEfiGraphicsOutputProtocolGuid)` and pushes each
    GOP device path into `ConOut` and `ErrOut` via
    `EfiBootManagerUpdateConsoleVariable`. Without this,
    `GraphicsConsoleDxe` binds but ConSplitter never routes any text to
    it and the panel stays black after the EDK2 banner.

### Console / debug

13. **DEBUG noise** — at `PcdDebugPrintErrorLevel=0x804FFFFF` the
    pool/load spam buries the Shell prompt. Lowered to `0x80000000`
    (ERROR only). Note: this **also masks `DEBUG_INFO`**, so when
    debugging your own driver, raise it back temporarily.

14. **Heredoc + `sshpass + sudo bash -s`** mangles bash escapes. Always
    write helper scripts to `/tmp`, scp them, then
    `ssh sudo bash /tmp/script.sh`. (See [`research/`](./research/).)

---

## Closed-BSP walls — current status

Allwinner A733 is **substantially more locked down than typical sun50i
parts**. As of v0.2 one of the three walls (USB) has been broken
through; PCIe and Ethernet remain.

### Wall 1 — USB host controllers ✅ EHCI working

**As of v0.2 the EHCI1 side is working.** Clean-room implementation of
the sun60iw2 CCU + USB2 PHY bring-up sequence in
[`SunxiUsbDxe.c`](Platform/OrangePi/OrangePi4ProPkg/Drivers/SunxiUsbDxe/SunxiUsbDxe.c).
Hubs, mass-storage devices, and HID boot-protocol keyboards all work on
EHCI1, which on the Orange Pi 4 Pro is wired to the **right pair of
USB-A ports** (top and bottom). EHCI0 (left bottom USB-A) still needs
an ordering fix — it shares its USB2 PHY with the OTG controller block
and we already do `OTG+0x420 &= ~BIT0` (`USBC_SelectPhyToHci`) but
likely in the wrong order relative to PHY reset. xHCI (left top USB-A,
USB 3.0) needs the **Cadence Combo PHY** serdes block at `0x06C00000`
+ `0x06C06000` plus the DWC3 stack — not started. The board’s USB-C
is power-only, so there is no OTG/device-mode work to do.

We also saved a complete CCU + USB PHY + USB controller register snapshot
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

- Same shape as USB pre-v0.2: BSP-only CCU clock IDs, BSP-only PHY init.
  Now that the CCU + PHY pattern is broken (see `SunxiUsbDxe.c`), this
  is the obvious next target.

---

## References

Sources used as **hardware-behaviour specifications** for clean-room
reimplementation in EDK2 (we read them, did not copy them — code in
this repo is fresh BSD-2-Clause-Patent, written in EDK2 style):

- [orangepi-xunlong/orange-pi-5.15-sun60iw2](https://gitee.com/orangepi-xunlong/orange-pi-5.15-sun60iw2)
  (BSP kernel, GPL-2.0) — `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.[ch]`,
  `bsp/drivers/usb/host/sunxi-hci.c` (USB PHY ordering — primary source
  for the v0.2 EHCI bring-up),
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
│   │   └── SunxiUsbDxe/                 # CCU + USB2 PHY bring-up + EHCI registration ✅
│   ├── Library/
│   │   └── PlatformBootManagerLib/      # console DP + GOP wiring + USB-KB ConIn + Shell boot option
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

## What's next

- [ ] **Fix EHCI0** (left-bottom USB-A) — share-PHY-with-OTG ordering
      quirk; `OTG+0x420 &= ~BIT0` already done but probably needs to
      happen before the PHY reset assert/deassert
- [ ] **xHCI (left-top USB-A, USB 3.0)** — Cadence Combo PHY at
      `0x06C00000`/`0x06C06000` + DWC3 controller stack
- [ ] **DesignWare PCIe** `PciHostBridgeLib` for sun60iw2 — find the
      DBI unlock register and re-enumerate
- [ ] **Ethernet** — apply the same CCU + PHY pattern from `SunxiUsbDxe`
- [ ] **Real `Variable` runtime services** backed by SPI NOR
- [ ] **Replace BSP `BL33` slot in SPI with EDK2 directly**
      (eliminates U-Boot from the chain)
- [ ] **ACPI table generator** for the A733 (so generic distros boot)
- [ ] **DTB hand-off path** to a Linux kernel via
      `\EFI\BOOT\BOOTAA64.EFI`

---

## Credits

- **Authored entirely by Claude Opus 4.7 (Anthropic).** All code, all
  debugging, all serial-log archaeology, all DSC/FDF surgery, all
  register reverse-engineering against running silicon.
- **Human supervision only.** Plugging in the SD card, pulling it out,
  pressing reset, reading back terminal output, swapping USB devices,
  and choosing which walls to bang our head against next.
- TianoCore EDK2 community for the upstream tree.
- Allwinner / Orange Pi for the BSP boot chain we currently chainload
  from and the BSP kernel sources we used as a hardware-behaviour
  spec.

## License

BSD-2-Clause-Patent (matches upstream EDK2).
