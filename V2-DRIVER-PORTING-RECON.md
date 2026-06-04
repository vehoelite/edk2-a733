# A733 v2 — Vendor-Driver → Modern-Kernel Porting Recon

**Goal:** drop the vendor-kernel crutch. Get a *modern mainline-class* kernel (Ubuntu's 6.x/7.0)
to drive the A733 (`sun60iw2p1`) itself, so stock Ubuntu boots with no BSP kernel.

**Why this is needed:** the generic Ubuntu kernel boots fine via our UEFI (8 CPUs, GIC, display,
initrd, casper) but finds **no block devices** — mainline has no driver matching the A733's
controllers (`compatible = "allwinner,sunxi-mmc-v5p3x"` etc.). USB/MMC/PCIe all fail to bind.

Source tree surveyed: `~/touchfix/build/ksrc` (BSP **5.15.147-sun60iw2**, NOT a git tree).
Target: Ubuntu **5.15.147 vendor kernel today → 6.x/7.0 mainline** next.

---

## The three blocking controllers (ranked by effort/value)

### 1. MMC / SMHC — **LOWEST EFFORT, HIGHEST VALUE. Start here.**
- BSP driver: `bsp/drivers/mmc/sunxi-mmc-v5px.c` (**610 LOC**, the `v5p3x` variant) + shared
  `bsp/drivers/mmc/sunxi-mmc.c` (4847 LOC core).
- **KEY FINDING — mainline ALREADY has a sunxi-mmc driver** (`drivers/mmc/host/sunxi-mmc.c`)
  supporting `sun4i…sun50i-a100-mmc`. The A733 SMHC is the same "new SMHC" generation as
  A100/D1. So this is likely **NOT a full port** — it's *"teach mainline sunxi-mmc the A733
  quirks"*: add an `allwinner,sun55i-a733-mmc` compatible + the right timing/clock-delay
  config, possibly the v5p3x register deltas. (We already tested faking `sun50i-a100-mmc`
  compatible → didn't bind, so it needs real quirk work, not just a string.)
- Effort: **small-to-medium.** Diff the BSP v5px register/timing handling vs mainline's, port
  the deltas as a new compatible. This single driver unblocks **SD + eMMC boot** for stock kernels.

### 2. PCIe + Cadence ComboPHY — **MEDIUM-HIGH EFFORT, HIGH VALUE (NVMe).**
- BSP: `bsp/drivers/pcie/` (`pcie-sunxi-rc.c` 875, `-plat.c` 1275, `-dma.c` 270) +
  `bsp/drivers/phy/sunxi-cadence-combophy.c` (**4372 LOC** — big).
- The PCIe RC is **custom** (refs `pcie_port`, not dwc/designware/cadence-pcie core) — so it's a
  fuller port, not a glue-to-existing-core job. The ComboPHY is Cadence-flavored but vendor-wrapped.
- **GOOD NEWS:** under the vendor kernel we *watched* `sunxi-cadence-combophy 6c00000.serdes:
  combophy0 set power ON` and `nvme0n1` enumerate — so the sequence is known-correct; we're
  forward-porting working code, not reversing.
- Effort: **the big one.** ~6900 LOC across PCIe+PHY. Do AFTER MMC (MMC gives a bootable stock
  kernel from SD; NVMe is the prize after).

### 3. USB PHY / host — **MEDIUM EFFORT, MEDIUM VALUE.**
- BSP: `bsp/drivers/usb/sunxi_usb/usbc/usbc_phy.c` (642 LOC) + the wider `sunxi_usb` tree.
- Needed for keyboard/USB-storage under a stock kernel (the "USB electrically dead" symptom).
- Mainline has `phy-sun4i-usb` / `phy-sun50i-usb3` — check if the A733 PHY is close enough to
  extend one of those vs. port the vendor one.

### Bonus — Ethernet (already solved): `sunxi-stmmac`
- `bsp/drivers/net/.../sunxi` glue over the **mainline `stmmac` core**. This is the EASY pattern
  and the proof-of-concept template: vendor glue + mainline core. Works today as a module.
  Likely already mainline-portable with a DT compatible — good warm-up / template.

---

## Strategy & sequencing
1. **Set up a build env** that compiles modules against a modern kernel (Ubuntu 6.x/7.0 headers).
   The vendor kernel is only 5.15; the real test is building these against the *target* kernel.
2. **Port MMC first** (smallest, mainline base exists, unblocks SD boot of a stock kernel).
   Validate: stock Ubuntu kernel + ported sunxi-mmc → `/dev/mmcblk1` appears → casper finds root.
3. **Then PCIe+ComboPHY** (the NVMe prize). Forward-port from the known-working BSP sequence.
4. **Then USB PHY.** 
5. Long game: clean each to upstream quality, submit to mainline → A733 support for everyone.

## Porting tax / notes
- BSP drivers lean on a few out-of-tree helpers (`sunxi-log.h`, `bsp/` includes) — light coupling
  (1 of the 3 sampled), stub-able during port.
- ksrc is NOT a git tree, so no easy `git diff` vs mainline — diff manually against the upstream
  `drivers/mmc/host/sunxi-mmc.c` etc.
- API drift 5.15→7.0 to expect: `gpiod`/`of_get_named_gpio`, `dma_` map API, `pm_runtime`,
  `platform_get_irq`, `class_create` signature, phy_ops, regulator/clk/reset bulk APIs.
- Dev loop is now FAST: `ssh root@192.168.0.244` (Ubuntu-on-NVMe) — mount BSP `opi_root` for the
  source, build, scp modules, test via EDK2 boot. No more serial puppeteering.
