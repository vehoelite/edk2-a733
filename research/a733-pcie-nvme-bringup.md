# A733 PCIe / NVMe bring-up — upstream sources and the authoritative init order

Research notes, 2026-08-20. The EDK2-side blocker has been "SunxiPcieDxe hangs in its
entry" with DBI at `0x06000000`. This documents where the real init sequence lives so
that work stops being blind register poking.

## The single most useful find: the vendor BSP driver, for this exact board

`orangepi-xunlong/linux-orangepi`, branch **`orange-pi-5.15-sun60iw2`**:

| file | size | what it gives you |
|------|------|-------------------|
| `bsp/drivers/pcie/pcie-sunxi-plat.c` | 33 KB | probe, power, **clock/reset order**, PHY hand-off, DBI accessors, LTSSM |
| `bsp/drivers/pcie/pcie-sunxi-rc.c` | 23 KB | root-complex setup, config space access, iATU, link training |
| `bsp/drivers/pcie/pcie-sunxi.h` | 15 KB | register definitions |
| `bsp/drivers/pcie/pcie-sunxi-ep.c` | 23 KB | endpoint mode (not needed for RC) |
| `bsp/drivers/pcie/pcie-sunxi-dma.c` | 7 KB | eDMA engine |
| `bsp/drivers/phy/sunxi-cadence-combophy.c` | **155 KB** | the Cadence Combo PHY — gates **both** PCIe and USB3/xHCI |

That last one matters twice over: the same PHY block is Wall 2 (PCIe/NVMe) and the xHCI
half of Wall 1. One driver port unblocks both.

## The init order (from `sunxi_pcie_plat_hw_init`)

```
sunxi_pcie_plat_power_on(pci)      // regulators: pcie3v3 -> 3.3V, pcie1v8 -> 1.8V
sunxi_pcie_plat_clk_setup(pci)     // resets then clocks, exact order below
sunxi_pcie_plat_combo_phy_init(pci)// phy_init() -> sunxi-cadence-combophy
```

and only after all three is DBI meaningfully accessible. `sunxi_pcie_plat_clk_setup`
for our variant does, in order:

```
reset_control_deassert(pcie_rst)      // "pclk_rst"
reset_control_deassert(pwrup_rst)     // "pwrup_rst"
clk_prepare_enable(pcie_aux)          // "pclk_aux"
clk_set_rate(pcie_slv, 400000000)     // "pclk_slv" -- 400 MHz, required on our variant
clk_prepare_enable(pcie_slv)
reset_control_deassert(pcie_its_rst)  // "its"
clk_prepare_enable(pcie_its)          // "its"
```

Then `sunxi_pcie_host_setup_rc()`, `sunxi_pcie_plat_ltssm_enable()` (set
`PCIE_LINK_TRAINING` in `PCIE_LTSSM_CTRL`), and wait for link.

**Working hypothesis for the DBI hang: DBI is being touched before the power domain,
resets, clocks and PHY are up.** Everything above must precede the first DBI read.

## What this board actually is (read from the live device tree)

`/proc/device-tree/soc@3000000/pcie@6000000`:

```
compatible     = allwinner,sunxi-pcie-v300-rc
reg            = 0x06000000 size 0x00480000   (reg-names = "dbi")
clock-names    = pclk_aux | pclk_slv | its        ids 0xE3, 0xE4, 0x32
reset-names    = pclk_rst | pwrup_rst | its       ids 0x59, 0x5A, 0x00
phy-names      = pcie-phy                         (Cadence Combo PHY)
power-domains  = <pck600 7>                       <-- PCK600 power domain, index 7
pcie3v3-supply / pcie1v8-supply
power-gpios / reset-gpios / wake-gpios
max-link-speed = 3 (Gen3), num-lanes = 1
num-ib-windows = 16, num-ob-windows = 16, num-edma = 4
```

`allwinner,sunxi-pcie-v300-rc` selects `sunxi_pcie_rc_v300_of_data`, i.e. **all four
flags set**: `has_pcie_slv_clk`, `need_pcie_rst`, `pcie_slv_clk_400m`,
`has_pcie_its_clk`. So none of the conditional branches above can be skipped.

Note the **`power-domains = <pck600 7>`** entry. `SunxiUsbDxe` already pokes the PCK600
PPU (`R_CCU+0x01AC`) for USB; PCIe needs its own domain enabled the same way, and that
is a strong candidate for the missing step before DBI responds.

Live clock rates with the vendor driver bound (from `clk_summary`), all enabled:

```
pcie0-aux         24000000
pcie0-axi-slv    400000000
its-pcie0-aclk    26000000
```

## Careful: two incompatible sets of clock/reset IDs

The vendor DT numbers (`0xE3`, `0xE4`, `0x32` / `0x59`, `0x5A`, `0x00`) are **BSP CCU
binding IDs and do not match mainline's**. Resolving them against mainline U-Boot's
`sun60i-a733-ccu.h` yields nonsense (`its` -> `CLK_BUS_G2D`). Always resolve vendor DT
numbers against the BSP headers, or better, read the registers off a running system.

## Other upstream material

- **A733 datasheet (public):**
  `https://dl.radxa.com/cubie/a7a/docs/hw/datasheet/A733_Datasheet_V0.93.pdf`
  Board schematics + component placement alongside it at `.../docs/hw/`.
- **Mainline U-Boot A733 series** (Yixun Lan, v2 2025-11-30, 10 patches):
  branch `https://github.com/dlan17/u-boot/tree/allwinner/A733/next`.
  Confirms values we had derived by measurement — `clk_a733_r.c` has
  `[CLK_BUS_R_UART0] = GATE(0x18c, BIT(0))`, and `sunxi_gpio.h` has
  `SUNXI_PINCTRL_BANK_SIZE 0x80` for A733. No PCIe in that series yet.
- **Linux mainline**: RFC series for A733 CCU + PRCM (`ccu-sun60i-a733.c`,
  `ccu-sun60i-a733-r.c`); RTC and PCK600 pmdomain patches are in flight. No PCIe.

So for PCIe the BSP is the only real source; mainline has nothing to copy yet.

## Suggested order of work

1. Enable the PCK600 power domain (index 7) for PCIe, mirroring what `SunxiUsbDxe`
   already does for USB.
2. Deassert `pclk_rst` and `pwrup_rst`, enable `pclk_aux`, set `pclk_slv` to 400 MHz and
   enable it, deassert and enable `its`. Capture the exact register writes by diffing
   CCU state with the vendor driver bound vs unbound on a running board — the same
   technique that identified the UART7 gate bit (`R_CCU 0x0701018C`, bit 0 / bit 16).
3. Port `sunxi-cadence-combophy` init. This is the large piece, and it also unblocks
   xHCI/USB3.
4. Only then touch DBI, then `setup_rc` -> `ltssm_enable` -> wait for link.
5. Keep the scoped DMA offset work from PR #1 in mind: inbound iATU must cover all DRAM
   if `EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE` is enabled, otherwise buffers above the
   window are unreachable by the device.

## Measured register recipe (CCU), 2026-08-20

Obtained empirically on a running board rather than read off a datasheet: snapshot the
CCU with the vendor `sunxi-pcie` driver bound (PCIe up, RC enumerated as `00:00.0`),
`unbind` it, snapshot again, diff. Reproduced identically on a second run. Same technique
that identified the UART7 gate bit.

CCU base is `0x02002000` (`/soc@3000000/ccu@2002000`, size `0x2000`).
R_CCU is `0x07010000` (size `0x340`) and **showed no changes at all**.

| register | PCIe UP | PCIe DOWN | vendor name |
|----------|---------|-----------|-------------|
| `CCU+0x0574` | `0x00010002` | `0x00000000` | `RST_BUS_ITS_PCIE0` = BIT(16), ITS-PCIe0 gate = BIT(1) |
| `CCU+0x1380` | `0x80000000` | `0x00000000` | `pcie0_aux` clock, BIT(31) = enable |
| `CCU+0x1384` | `0x82000000` | `0x02000000` | `pcie0_axi_slv` clock, BIT(31) = enable (`0x02000000` is the retained source/divider field) |
| `CCU+0x138C` | `0x00030000` | `0x00000000` | `RST_BUS_PCIE0` = BIT(17), `RST_BUS_PCIE0_PWRUP` = BIT(16) |
| `CCU+0x1B28` | `0x00020000` | `0x00010000` | **unattributed** — see caveat |

The first four are confirmed independently in
`bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c`:

```c
0x0574, BIT(1), 0);                              /* ITS-PCIe0 gate      */
[RST_BUS_ITS_PCIE0]   = { 0x0574, BIT(16) },
pcie0_aux_parents,     0x1380,
pcie0_axi_slv_parents, 0x1384,
[RST_BUS_PCIE0]       = { 0x138c, BIT(17) },
[RST_BUS_PCIE0_PWRUP] = { 0x138c, BIT(16) },
```

so the measured bits and the vendor tables agree exactly.

**Caveat on `CCU+0x1B28`.** It changes deterministically (reproduced on a repeat run) but
holds BIT(17) when up and BIT(16) when down — one bit set in *each* state, which is not
how a clock gate behaves — and no BSP driver under `bsp/drivers/{pcie,power,irqchip}` or
the CCU/PHY drivers references that offset. Best current reading is a **status register
reflecting PCIe power/reset state**. Do not write it; treat it as a probe point.

### Bring-up write sequence for EDK2

Reverse of the teardown, following `sunxi_pcie_plat_clk_setup()` ordering:

```c
// 1. resets first: RST_BUS_PCIE0 | RST_BUS_PCIE0_PWRUP
MmioOr32 (0x0200338C, BIT17 | BIT16);
// 2. pcie0_aux clock enable
MmioOr32 (0x02003380, BIT31);
// 3. pcie0_axi_slv: preserve the source/divider field, set enable.
//    Vendor sets this rate to 400 MHz (pcie_slv_clk_400m); the observed
//    retained value 0x02000000 is what a working system holds.
MmioOr32 (0x02003384, BIT31);
// 4. ITS-PCIe0 reset deassert + gate
MmioOr32 (0x02002574, BIT16 | BIT1);
```

Then regulators (`pcie3v3`, `pcie1v8`), the Cadence Combo PHY (`phy_init`), and only then
DBI at `0x06000000`, `setup_rc`, `ltssm_enable`.

**Caveat on ordering.** The vendor driver deasserts resets *before* enabling clocks, and
sets `pclk_slv` to 400 MHz *before* enabling it. The snapshot cannot show ordering or
intermediate rate programming — only the final state — so follow
`sunxi_pcie_plat_clk_setup()` for sequence and use the table above for values.

**Caveat on regulators and power domain.** R_CCU showed no change across bind/unbind, so
this diff says nothing about the PCK600 power domain (`power-domains = <pck600 7>`) or
the `pcie3v3`/`pcie1v8` regulators — those are presumably left enabled by boot and are
not touched by driver bind/unbind. EDK2 may still need to enable them explicitly if
U-Boot does not. That remains unverified.

## Device-tree facts, read off the running board (2026-08-21)

Taken from  on the vendor 5.15.147-sun60iw2 kernel with a
WD_BLACK SN7100 fitted and the link up. Every phandle below was resolved by
walking the tree, so these are the boards

## Device-tree facts, read off the running board (2026-08-21)

Taken from `/proc/device-tree` on the vendor 5.15.147-sun60iw2 kernel with a
WD_BLACK SN7100 fitted and the link up. Every phandle below was resolved by
walking the tree, so these are the board's real bindings rather than a guess
carried over from a similar SoC.

### `pcie@6000000` (`allwinner,sunxi-pcie-v300-rc`)

| property | value | resolves to |
| --- | --- | --- |
| `reg` | `0x06000000` len `0x00480000` | DBI (the only reg; no separate ATU reg) |
| `ranges` | windows at `0x20000000`, `0x21000000`, `0x22000000` | matches `pcie@6000000` at `22000000-27ffffff` in `/proc/iomem` |
| `max-link-speed` / `num-lanes` | `3` / `1` | Gen3 x1 |
| `num-ib-windows` / `num-ob-windows` | `16` / `16` | iATU window count |
| `clocks` | `<ccu 227> <ccu 228> <ccu 50>` | `pclk_aux`, `pclk_slv`, `its` |
| `resets` | `<ccu 89> <ccu 90> <ccu 0>` | `pclk_rst`, `pwrup_rst`, `its` |
| `power-domains` | `<pck600 7>` | `/pck-600@7060000/power-controller` |
| `phys` | `<0x14c>` | `serdes@6c00000/combo-phy1@6c02000/combo1-pcie-phy` |
| `pcie3v3-supply` | `<0x4c>` | `bldo1` on the PMU at `twi@7083000/pmu@36` |
| `pcie1v8-supply` | `<0x5f>` | `dcdc1`, same PMU |
| `power-gpios` | `<r-pinctrl 0 3 0>` | **PL3**, active high |
| `reset-gpios` | `<pinctrl 3 22 0>` | **PD22** = PERST#, active high |
| `wake-gpios` | `<pinctrl 3 21 0>` | **PD21** |

The three GPIOs and the two regulators are exactly the part a CCU bind/unbind
diff can never show, because they are not in the CCU at all. They are also the
part most likely to be left already configured by U-Boot, which is why the
earlier diff looked complete when it was not.

Confirmed against the kernel's own view, which agrees on all three pins:

```
gpiochip0 (2000000.pinctrl):  gpio-117 |wake ) out hi     <- PD21
                              gpio-118 |reset) out hi     <- PD22, PERST# released
gpiochip1 (7025000.pinctrl):  gpio-355 |power) out hi     <- PL3

/sys/kernel/debug/pinctrl/2000000.pinctrl/pinmux-pins:
    pin 117 (PD21): GPIO 2000000.pinctrl:117
    pin 118 (PD22): GPIO 2000000.pinctrl:118
```

### The Cadence combo PHY

`serdes@6c00000` is `allwinner,cadence-combophy`, with three reg ranges:

| range | purpose |
| --- | --- |
| `0x06c00000` len `0x0400` | serdes common |
| `0x06c06000` len `0x2000` | serdes common, second block |
| `0x0709016c` len `0x0004` | a single register out in the PRCM area, almost certainly a serdes select/mux |

clocks `<ccu 229>` (`serdes-clk`) plus two dcxo clocks from phandle `0x2b`;
reset `<ccu 91>` (`serdes-reset`).

It has two children. PCIe uses **combo-phy1**:

| node | reg |
| --- | --- |
| `combo-phy0@6c01000` | `0x06c01000` len `0x0a00`, `0x06c80000` len `0x20000` |
| `combo-phy1@6c02000` | `0x06c02000` len `0x0a00`, `0x06ca0000` len `0x20000` |
| `aux-hpd@6c01e00` | `0x06c01e00` len `0x0200` |

combo-phy1 is shared by `combo1-usb-phy` and `combo1-pcie-phy`, and combo-phy0
by `combo0-usb-phy` and `combo0-dp-phy`. That sharing is why porting this one
driver also unblocks USB3 and DisplayPort.

### PIO bank layout, main pinctrl

Driving PL3 and PD22 from EDK2 needs the register offsets, and the main
pinctrl does **not** use the same base rule as R_PIO.

R_PIO (`0x07025000`): bank PL is at offset `0x00`. Our working UART7 console
proves this. The code muxes PL6/PL7 by writing `0x07025000`, and reading it
back shows `cfg0 = 0x33ff1f22`, i.e. PL6 and PL7 both at mux 3 (UART7).

Main PIO (`0x02000000`): offset `0x00` is a **table of bank offsets**, not a
pin bank. It reads `000, 100, 180, 200, 280, 300, 380, 400, 480, 500, 580` for
PA through PK, so **PD is at `0x02000200`**.

The table is not a constant stride. PA sits at `0x000` and PB at `0x100`, and
only from PB onward does it step by `0x80`. Any formula of the form
`k + N*0x80` reproduces PB..PK correctly and gets **PA wrong**, so read the
table rather than computing an offset. This also explains the apparent
asymmetry with R_PIO: there is no shift, PA really is at offset 0 in both.

The bank positions were fingerprinted independently against the pins the kernel
names, and all six agree with the table:

| pin, per kernel | predicted block | mux read | ok |
| --- | --- | --- | --- |
| PB6, PB7 output | `0x100` | 1, 1 | yes |
| PB8 output | `0x100` cfg1 | 1 | yes |
| PD21, PD22 output | `0x200` | 1, 1 | yes |
| PF6 input + IRQ | `0x300` | 0xE (EINT) | yes |

`0x02000200` is also the only block in the whole `0x1000` region with both data
bit 21 and bit 22 set, which is what "PD21 and PD22 both driven high" requires.

This layout was already established on 2026-08-20 while chasing the UART0
pinmux defect, and the table at offset 0 is the documented form. An earlier
draft of this section described it as `0x80 + N*0x80` with an unexplained
offset-0 block. That formula happens to give the right answer for PB..PK, which
is why it survived the fingerprint check, but it is wrong for PA. Use the
table.

### Hazard

Do not read the DBI or PHY regions after unbinding the PCIe driver. The unbind
gates the clocks, and a read with the clocks gated is a bus hang, not a zero.
Snapshots of those regions have to be taken in the link-up state only.

## The U-Boot A733 PCIe driver is the shortcut (2026-08-21)

The vendor U-Boot on this board has PCIe compiled out, so **U-Boot does not
train the link and EDK2 inherits nothing**. From
`/usr/lib/u-boot/sun60iw2p1_t736_defconfig` on the board:

```
# CONFIG_PCI is not set
# CONFIG_AW_CADENCE_COMBOPHY is not set
CONFIG_PCIE_PERST_GPIO=""
CONFIG_PCIE_WAKE_GPIO=""
CONFIG_PCIE_POWER_GPIO=""
```

That kills the cheapest hypothesis, which was that we could just enumerate a
link U-Boot had already brought up.

It does however prove those symbols exist in Allwinner U-Boot, and a public
tree carries them for our exact SoC: `JasonYANG170/ZeroA733-Uboot` has
`src/configs/sun60iw2p1_a733_defconfig` with `CONFIG_PCI=y`, `CONFIG_NVME=y`,
`CONFIG_PCIE_ALLWINNER_RC=y`, `CONFIG_AW_CADENCE_COMBOPHY=y`. Same SoC,
different board, so the *sequence* is ours to reuse and the *pins* are not
(theirs are PE11/PE12/PE13, ours are PL3/PD21/PD22 as measured above).

The four relevant files, ~3000 lines total, versus 155KB for the Linux BSP
driver:

| file | lines | role |
| --- | --- | --- |
| `drivers/pci/pcie-sunxi-plat.c` | 430 | clocks, resets, regulators, LTSSM |
| `drivers/pci/pcie_sunxi_rc.c` | 485 | RC core, iATU, enumeration |
| `drivers/pci/pcie-sunxi.h` | 481 | register definitions |
| `drivers/phy/allwinner/sunxi-cadence-combophy.c` | 1664 | the PHY |

**Licensing.** U-Boot is GPL-2.0 and this tree is BSD-2-Clause-Patent. Those
files are kept out of the repo deliberately. Use them as documentation of the
hardware -- register offsets and ordering are facts about the silicon, not
expression -- and write our own implementation from the facts.

### The measured CCU recipe is confirmed by the U-Boot source

`sunxi_pcie_plat_clk_setup()` does exactly what the bind/unbind diff measured,
in the same order:

| U-Boot step | measured register |
| --- | --- |
| `pcie_bgr_reg \|= RST \| PWRUP_RST` | `CCU+0x138C \|= BIT17\|BIT16` |
| `pcie_aux_clk_reg \|= GATING` | `CCU+0x1380 \|= BIT31` |
| `pcie_axi_slv_clk_reg \|= (2<<24)`, then GATING | `CCU+0x1384` |
| `its_bgr_reg \|= ITS_RST \| ITS_GATING` | `CCU+0x0574 \|= BIT16\|BIT1` |

One correction to the earlier notes: the `0x02000000` seen at `CCU+0x1384` was
recorded as "retained src/div". It is not retained, it is written explicitly --
`(2 << 24)` is the 400MHz clock source select, guarded by `pcie_slv_clk_400m`.

Init order overall is **regulators -> resets and clocks -> Cadence PHY**, and
only then DBI. U-Boot never touches the pck600 power domain, which suggests
domain 7 is on out of reset and is one less thing for us to bring up.

### Register facts confirmed against our silicon

`app_base = dbi_base + PCIE_USER_DEFINED_REGISTER` where that offset is
`0x400000`, so the app block is at **`0x06400000`**, inside the DBI window.
This is why the DT declares only one reg range. Verified by reading the live
board with the link up:

```
app+0xC00 LTSSM_CTRL = 0x00000041   = PCIE_LINK_TRAINING(BIT0) | DEVICE_TYPE_RC(BIT6)
app+0xE0C LINK_STAT  = 0x00000013   = SMLH_LINK_UP | RDLH_LINK_UP
app+0x800 PHY_CFG    = 0x00a023f0   (value with a trained Gen3 x1 link)
```

Both reads decode exactly as the header predicts, which confirms the offset on
A733 rather than on a cousin SoC.

Address windows also cross-check against our DT `ranges` and `/proc/iomem`:

| window | U-Boot | ours |
| --- | --- | --- |
| cfg | `0x20000000` size `0x01000000` | `ranges` entry 1 |
| io | `0x21000000` size `0x01000000` | `ranges` entry 2 |
| mem | `0x22000000` size `0x07000000` | `/proc/iomem` `22000000-27ffffff` |

The iATU is the unrolled DesignWare form at `dbi + 0x300000 + n*0x200`, which
is what makes the DBI window `0x480000` long.

### Open question: the core DBI reads all-ones under Linux

Scanning the whole `0x480000` DBI window on the running board, only
`0x380000..0x47ffff` reads as anything other than `0xffffffff`. Config space at
`+0x000` and the iATU at `+0x300000` both read all-ones, while the app block at
`+0x400000` reads real values.

`0xffffffff` is the normal PCIe unsupported-request fill, and
`PCIE_ADDR_PAGE_CFG` (app+0x04) reads 0, so the lower part of the window may be
a paged view rather than flat DBI. U-Boot's driver assumes flat access from a
cold start and never programs a page register, so this is most likely an
artifact of the state the Linux driver leaves behind rather than something we
must replicate.

Flagging it as unresolved rather than concluding it. If our EDK2 driver brings
the block up and config reads still return all-ones, `ADDR_PAGE_CFG`,
`AWMISC_CTRL` (app+0x200) and `ARMISC_CTRL` (app+0x220) are the first three
registers to look at -- all three currently read 0.

Also note the header defines `PCIE_CTRL_MGMT_BASE 0x900000`, which is past the
end of our `0x480000` DBI window. That header covers several SoCs, so not every
constant in it applies to A733. Check each against the window size before use.

## The Cadence combo PHY PCIe sequence (2026-08-21)

Most of the 1664 line U-Boot PHY driver is DisplayPort and USB3 register
tables. The PCIe path is roughly 190 lines of flat MMIO, which makes it a
realistic EDK2 port rather than a rewrite.

`sunxi_cadence_phy_combo1_pcie_init()` in order:

1. `clk_set_rate(serdes_clk, 100000000)` then enable it
2. enable `dcxo_serdes1_clk` (the `DCXO_SERDES1_GATING BIT(5)` bit, which is
   almost certainly the lone `0x0709016c` register in the serdes DT reg list)
3. `subsys + 0x04 |= BIT16|BIT17|BIT18` -- `SUBSYS_PCIE_BGR`
4. `subsys + 0xf0 |= BIT29` -- `SUBSYS_DISABLE_COMBO1_AUTOGATING`
5. `combo + 0xc44 = 1` -- `SUBSYS_COMB1_PIPE_PCIE`, the mux that points
   combo-phy1 at PCIe rather than USB3
6. `sunxi_cadence_phy_pcie_phy_init()`, the register table

The table itself is mostly 16-bit writes into the PHY array, with three
read-modify-writes near the end, then:

```
top_reg + 0x000 |= BIT0
top_reg + 0x100 |= BIT0
poll top_reg + 0x900 until BIT0 is set     <- PMA ready, with 100us between reads
phy_reg + 0x0a0  = 0x270
phy_reg + 0x098 |= BIT4
phy_reg + 0x18000 |= BIT0
top_reg + 0x004 |= BIT28
```

### The register bases all match our DT

This is worth stating because it is the independent check that this driver is
for our silicon and not a cousin:

| U-Boot field | offsets it uses | our DT range |
| --- | --- | --- |
| `top_subsys_reg` | `0x04`, `0xf0` | `0x06c00000` len `0x400` |
| `top_combo_reg` | `0xc44` | `0x06c06000` len `0x2000` |
| `combo1->top_reg` | `0x00`..`0x900` | `0x06c02000` len `0xa00` |
| `combo1->phy_reg` | up to `0x18000` | `0x06ca0000` len `0x20000` |

Every offset fits inside its range, and `0xc44` does not fit in `0x400`, which
is what forces `top_combo_reg` to be the `0x06c06000` block rather than the
other way round.

## Remaining plan for SunxiPcieDxe

Everything below is register-level settled except step 1.

1. regulators: `bldo1` to 3.3V and `dcdc1` to 1.8V, both on the AXP PMU behind
   I2C `twi@7083000`. **Open question** -- if they come up on their own we can
   skip an I2C driver entirely, and that is the one thing still worth checking
   on the board.
2. drive **PL3** high (power)
3. CCU clocks and resets, per the measured and now source-confirmed recipe
4. serdes and PHY, per the sequence above
5. **PD22** PERST#: assert low, delay, release high
6. LTSSM: `app+0xC00 |= BIT0`, then poll `app+0xE0C` for `SMLH|RDLH`
7. iATU outbound windows at `dbi + 0x300000 + n*0x200`, then hand off to the
   stock EDK2 `PciHostBridgeDxe` and `NvmExpressDxe`

Note for step 7: the NVMe fitted is a DRAM-less WD_BLACK SN7100, which leans on
the Host Memory Buffer. `NvmExpressDxe` does not set up HMB, so expect it to be
slower than under Linux. Not a blocker, just do not read the throughput as a
sign something is wrong.

### Hazard, learned the hard way: do not sweep the DBI window

Reading every 4K page of the `0x480000` DBI window through `/dev/mem` hung the
board hard on 2026-08-21. No ping on either address, and the UART7 console went
silent rather than printing a panic, which means an external abort with the CPU
wedged rather than a kernel oops. It needed a power cycle.

The scan did return useful information before it died -- only
`0x380000..0x47ffff` reads as anything other than all-ones -- but the same
answer was available by sampling the handful of offsets that the U-Boot header
actually names. Sample named offsets; do not sweep a window whose decode map
you do not know.

The safe offsets in this window, all verified readable with the link up:

```
app+0x000  PCIE_VER          app+0x800  PCIE_PHY_CFG
app+0x004  PCIE_ADDR_PAGE_CFG   app+0xC00  PCIE_LTSSM_CTRL
app+0x200  PCIE_AWMISC_CTRL     app+0xE04  PCIE_INT_ENABLE_CLR
app+0x220  PCIE_ARMISC_CTRL     app+0xE0C  PCIE_LINK_STAT
```

where `app = 0x06400000`.
