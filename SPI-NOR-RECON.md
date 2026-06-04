# A733 SPI-NOR Recon — toward UEFI variable persistence (and firmware-in-flash)

**What SPI-NOR unlocks:**
1. **Persistent UEFI variables** (BootOrder, Boot####, Setup options survive reboot) — we
   currently have **no variable backend** at all.
2. Longer term: **EDK2 itself living in SPI-NOR** so the board boots standalone (no SD/U-Boot
   chainload) — the "real computer" config.

## Hardware (confirmed on the board)
- **16 MB SPI-NOR flash**, kernel-enumerated as **`mtd0` ("spi0.0")**, 64 KB erase blocks
  (`mtd0: 01000000 00010000`). Chip type: generic `spi-nor` (SFDP-detectable).
- **SPI controller:** `spi@2540000` (spi0), `compatible = "allwinner,sunxi-spi-v1.3"`.
  (spi1..5 also exist: 0x2541000…0x2544000, 0x7092000.)
- ⚠️ **The SPI-NOR is the LIVE BOOT DEVICE** — but **NOT brickable.** Offset 0x0 = Allwinner
  **`eGON.BT0`** (the BROM loads this first); boot0 + U-Boot/ATF occupy roughly the low 1–2 MB
  (non-empty data through 0x100000+). **However, the A733 BROM tries SD-card BEFORE SPI-NOR**,
  so corrupting the SPI boot chain just falls through to a bootable SD card — recover by
  inserting an SD with the vendor boot chain and re-flashing SPI from Linux. **No external
  flasher / JTAG needed.** Worst case = "boot from SD, re-write mtd0." This makes SPI experiments
  much safer than typical NOR-boot boards. Still: prefer a **high, verified-empty region** for
  the variable store and keep a known-good SD boot ready.

## The good news: EDK2 already ships the NOR-flash layer
The board's EDK2 tree has **`MdeModulePkg/Bus/Spi/SpiNorFlashJedecSfdp/`**:
- `SpiNorFlashJedecSfdpDxe.c` — generic SPI-NOR **DXE driver**, **SFDP auto-detects** chip
  geometry/commands. We do NOT write a flash driver.
What's missing underneath it: an **A733 SPI host-controller driver** providing
`EFI_SPI_HC_PROTOCOL` (the transport the SFDP driver sits on).

## Work breakdown
1. **`SunxiSpiDxe`** — port the `allwinner,sunxi-spi-v1.3` controller (base `0x02540000`) to an
   `EFI_SPI_HC_PROTOCOL` DXE driver. Source the register sequence from BSP
   `bsp/drivers/mtd/spi-nor-6.1/controllers/sunxi-spif.c` (2129 LOC) and/or SyterKit's A733 SPI
   (same state-replay/PIO playbook that worked for `SunxiMmcDxe`). Likely **state-replay**:
   U-Boot already inits the SPI controller (it booted from it), so reuse its state + do PIO
   transfers.
2. **Wire `SpiNorFlashJedecSfdpDxe`** on top → read/write/erase the NOR via SFDP.
3. **UEFI variable store:** put `FaultTolerantWriteDxe` + `VariableRuntimeDxe` (NV variable
   region) on a **high, empty** SPI-NOR window. Verify the window is 0xFF-filled first.
4. (Future) **Firmware-in-flash:** lay out SPI-NOR as ATF + EDK2(FV) + a UEFI variable region,
   built as the BROM/SPL payload, so EDK2 is the first-stage boot from SPI — no SD chainload.

## Risk / method
- **Read-only first.** Bring up `SunxiSpiDxe` + SFDP read, dump the chip, map the *exact* empty
  region before ANY write. Confirm against `/proc/mtd` + `dd if=/dev/mtd0` offsets.
- The board boots from this chip; treat every erase as potentially board-bricking until the
  target window is proven safe and the recovery path (SD boot / external flasher) is ready.
- Dev loop: `ssh root@192.168.0.244`, mount BSP `opi_root` for `sunxi-spif.c`, build the DXE,
  test via EDK2 boot. Cross-check writes against the kernel's `mtd0` view.
