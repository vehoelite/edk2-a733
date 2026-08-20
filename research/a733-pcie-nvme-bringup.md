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
