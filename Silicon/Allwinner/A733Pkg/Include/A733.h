/** @file
  Allwinner A733 SoC memory map and hardware constants.

  Values marked TODO must be verified against the A733 datasheet / TRM.
  Cross-reference with the A523/T527 TRM (structurally similar big.LITTLE
  sunxi SoC) where possible.

  Copyright (c) 2024, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_H_
#define A733_H_

// ---------------------------------------------------------------------------
// DRAM
// ---------------------------------------------------------------------------
// Allwinner sunxi chips map DRAM starting at 0x40000000.
#define A733_DRAM_BASE          0x40000000UL
// TODO: confirm total DRAM size from your DDR init code / ATF platform file.
// Orange Pi 4 Pro ships with 4/6/8/12 GB LPDDR5.
#define A733_DRAM_SIZE_MAX      0x300000000UL   // 12 GB upper bound

// ---------------------------------------------------------------------------
// UART (NS16550 / DesignWare APB UART, 32-bit register stride)
// Confirmed from André Przywara's DTS gist (ARM Ltd, 2025-09-04) and
// linux-sunxi.org A733 wiki.  UART0 TX=PB0, RX=PB1 (pinmux func 3).
// ---------------------------------------------------------------------------
#define A733_UART0_BASE         0x02500000UL    // CONFIRMED — debug UART, PB0/PB1
#define A733_UART1_BASE         0x02500400UL
#define A733_UART2_BASE         0x02500800UL
#define A733_UART3_BASE         0x02500C00UL
#define A733_UART4_BASE         0x02501000UL
#define A733_UART5_BASE         0x02501400UL

#define A733_UART_REG_STRIDE    4               // 32-bit register spacing (reg-shift=2)
#define A733_UART_CLOCK_HZ      24000000UL      // 24 MHz HOSC (confirmed from DTS clocks)
// NOTE: OPi 4 Pro exposes UART0 on the 40-pin header (PB0=TX, PB1=RX).

// ---------------------------------------------------------------------------
// GIC-600 r1p4 — GICv3 with ITS
// Confirmed from André Przywara's DTS gist and linux-sunxi.org A733 wiki.
//   GICD = 0x03400000 size 0x10000
//   GICR = 0x03460000 size 0x100000  (8 cores × 0x20000)
//   ITS  = 0x03440000 size 0x20000
// ---------------------------------------------------------------------------
#define A733_GICD_BASE          0x03400000UL    // CONFIRMED
#define A733_GICR_BASE          0x03460000UL    // CONFIRMED — Redistributor frame 0
#define A733_GICR_STRIDE        0x00020000UL    // 128 KB per CPU interface
#define A733_GICR_SIZE          (8 * A733_GICR_STRIDE)  // 8 cores total
#define A733_GIC_ITS_BASE       0x03440000UL    // CONFIRMED — MSI ITS
#define A733_GIC_ITS_SIZE       0x00020000UL

// ---------------------------------------------------------------------------
// ARM Generic Timer
// The A733 (Cortex-A76/A55) uses the architectural system-register timer.
// No MMIO base is required; PL031 / SBSA timer are separate peripherals.
// ---------------------------------------------------------------------------
// TODO: read CNTFRQ_EL0 at runtime or pin it to 24000000 (24 MHz HOSC).
#define A733_TIMER_FREQUENCY_HZ 24000000UL

// ---------------------------------------------------------------------------
// Clock / Reset Controllers
// Confirmed from live DTB: ccu@2002000, cpupll_ccu@8870000, r_ccu@7010000
// ---------------------------------------------------------------------------
#define A733_CCU_BASE           0x02002000UL    // CONFIRMED — main CCU
#define A733_CPUPLL_CCU_BASE    0x08870000UL    // CONFIRMED — CPU PLL CCU
#define A733_R_CCU_BASE         0x07010000UL    // CONFIRMED — always-on CCU
#define A733_RTC_CCU_BASE       0x07090000UL    // CONFIRMED — RTC CCU
#define A733_PCK600_BASE        0x07060000UL    // CONFIRMED — DynamIQ PCK-600 power ctrl

// ---------------------------------------------------------------------------
// Pin Controller (PIO / R_PIO)
// Confirmed: PIO=0x02000000, R_PIO=0x07025000 (from DTS gist)
// ---------------------------------------------------------------------------
#define A733_PIO_BASE           0x02000000UL    // CONFIRMED
#define A733_R_PIO_BASE         0x07025000UL    // CONFIRMED — always-on domain pins
#define A733_R_I2C0_BASE        0x07083000UL    // CONFIRMED — PMIC (AXP8191) bus
#define A733_R_I2C1_BASE        0x07085000UL    // CONFIRMED

// ---------------------------------------------------------------------------
// Watchdog / Ethernet / DMA / IOMMU
// Confirmed from live DTB
// ---------------------------------------------------------------------------
#define A733_WDT_BASE           0x02050000UL    // CONFIRMED — watchdog@2050000
#define A733_EMAC_BASE          0x04500000UL    // CONFIRMED — ethernet@4500000
#define A733_DMA_BASE           0x04601000UL    // CONFIRMED — dma-controller@4601000
#define A733_IOMMU_BASE         0x03900000UL    // CONFIRMED — iommu@3900000
#define A733_DSU_FREQ_BASE      0x08860000UL    // CONFIRMED — DynamIQ DSU freq ctrl

// ---------------------------------------------------------------------------
// USB
// Confirmed from live DTB: xhci2-controller@6a00000, phy@6b00000
// ---------------------------------------------------------------------------
#define A733_USB_OTG_BASE       0x04100000UL    // USB OTG (not in live iomem, likely TODO)
#define A733_XHCI_BASE          0x06A00000UL    // CONFIRMED — USB3 xHCI controller
#define A733_USB2_PHY_BASE      0x06B00000UL    // CONFIRMED — USB2 PHY (u2_base)
#define A733_SERDES_BASE        0x06C00000UL    // CONFIRMED — SerDes (USB3/PCIe shared PHY)

// ---------------------------------------------------------------------------
// PCIe
// Confirmed from live DTB: pcie@6000000 (DBI base)
// ---------------------------------------------------------------------------
#define A733_PCIE_BASE          0x06000000UL    // CONFIRMED — PCIe DBI base

// ---------------------------------------------------------------------------
// eMMC / SD (SMHC — compatible with allwinner,sun20i-d1-mmc)
// Confirmed: MMC0=0x04020000 (SD card, PF0-PF5), from DTS gist.
// ---------------------------------------------------------------------------
#define A733_SMHC0_BASE         0x04020000UL    // CONFIRMED — SD0 (microSD, PF0-PF5)
#define A733_SMHC1_BASE         0x04021000UL    // CONFIRMED — SD1
#define A733_SMHC2_BASE         0x04022000UL    // CONFIRMED — SD2 / eMMC (sdmmc@4022000)

// ---------------------------------------------------------------------------
// Secure Boot / SID (efuse)
// ---------------------------------------------------------------------------
#define A733_SID_BASE           0x03006000UL

// ---------------------------------------------------------------------------
// Interrupt numbers (GICv3 SPI — hardware SPI N maps to GIC INTID N+32)
// Confirmed from DTS gist (0 N 4 = SPI N, level-high).
// ---------------------------------------------------------------------------
#define A733_UART0_IRQ          (32 + 2)        // CONFIRMED — SPI 2
#define A733_SD0_IRQ            (32 + 161)      // CONFIRMED — SPI 161
#define A733_PIO_IRQ_BASE       (32 + 69)       // CONFIRMED — SPI 69,71,73... (per bank)
#define A733_R_PIO_IRQ_BASE     (32 + 198)      // CONFIRMED — SPI 198, 200
#define A733_R_I2C0_IRQ         (32 + 203)      // CONFIRMED — SPI 203

// ---------------------------------------------------------------------------
// PSCI / ATF
// EDK2 will use SMC calls to TF-A BL31 for CPU on/off and system reset.
// PSCI 1.0 is expected; TF-A sunxi platform must be built alongside this.
// ---------------------------------------------------------------------------
#define A733_PSCI_VERSION       0x00010000UL    // PSCI v1.0

#endif // A733_H_
