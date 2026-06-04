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

## SunxiSpiDxe bring-up status (2026-06-03, 8 hardware iterations)
A native EDK2 SPI master (`Drivers/SunxiSpiDxe/`) was written and debugged on
real silicon over the serial console. **What's proven working on hardware:**
- Controller reached + alive: `VER=0x00010003` (was 0 until clocked).
- **CCU clock + reset** of SPI0 (U-Boot leaves it unclocked → MMIO reads 0;
  CCU @0x02002000, clk gate 0x0F00 bit31, bus gate 0x0F04 bit0, reset bit16).
- **Pinmux** PC2/PC3/PC4 → spi0 **function 5** (PIO @0x02000000, PC_CFG0 0x40;
  value verified against the live Linux mux 0x01155550).
- **SPI mode 0** (CPOL=0/CPHA=0), SPOL=1, CS0, software-owned SS — matched to
  the live working TC=0x1c4 (we were in mode 3 → flash returned zeros).
- **Transfer mechanics**: MBC/MTC/BCC burst counts, TC.XCH starts the burst and
  self-clears, RX FIFO captures all MBC bytes (skip first TxLen when draining),
  INT_STA shows TC (transfer-complete), no error bits.

**The ONE remaining piece — MISO sample timing on the MAIN FIFO path.** Reads
still return 0x00: the transfer completes and bytes land in the RX FIFO, but
they're zero, i.e. MISO is latched at the wrong instant. **Correction:** the
BSP's `sunxi_spi_bit_sample_delay()` lives in the **bit-bang `bit/` path**
(uses BATC/RB/TB regs — a *different* register set) and is NOT in our normal
FIFO path (MBC/MTC/BCC/TXDATA/RXDATA). So the fix is almost certainly NOT the
heavy calibrate routine — it's the **sample-control bits in the MAIN TC reg**:
`SDC` (BIT11 master sample-data-control), `SDM` (BIT13 sample-data-mode),
`SDDM` (BIT14), `SDC1` (BIT15) — set per clock speed. Compare our transfer-time
TC against the live working value with those bits, and/or confirm `SpiResetFifo`
isn't clobbering `FIFO_CTL`. This is a small, well-bounded fix — best done fresh.
**Then:** READ_ID returns the real XM25QU128C id (`0x20 0xBA 0x18` = XMC, 16MB),
read path complete → wire EFI_SPI_HC_PROTOCOL → SpiNorFlashJedecSfdp → NV vars.

### State for resuming (so next session lands it in ~1 iteration)
- Live working SPI0 regs (read via `/dev/mem @0x02540000` under Linux):
  `VER=0x00010003 GC=0x83 TC=0x01c4 CCR=0x02 SAMP_DL=0x2000 FIFO_CTL=0x00200140`.
- Our last on-HW transfer: `GC=0x83 TC=0x144 MBC=4 MTC=1 BCC=1`, XCH self-clears
  (timeout≈17), `FIFO_STA=3` (3 bytes captured), `INT_STA` shows TC, no errors,
  bytes = 0x00. So: everything works *except* the latched value.
- **Diff to investigate first:** live `TC=0x1c4` vs our `TC=0x144` → live has
  **BIT7 (SS_LEVEL=1, idle)** AND we drive it low for the burst (correct), but
  recheck the **upper sample bits** the live driver sets during an actual xfer
  (read TC live *during* a transfer, not just at idle). Likely just need
  `TC |= SDC` (BIT11) and/or `SDM` (BIT13) for this clock.
