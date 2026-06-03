/** @file
  SunxiUsbDxe - bring up A733 USB2 PHY, gate clocks, deassert resets,
  then publish the EHCI host controllers as non-discoverable devices
  so the EDKII EhciDxe driver attaches to them.

  Confirmed bases (from Linux /proc/iomem on this exact board):
     0x02002000  CCU         (size 0x2000)  main clock controller
     0x04101000  EHCI0       (size 0x400)
     0x04200000  EHCI1       (size 0x400)
     0x06A00000  xHCI2       (size 0x8000)  USB 3.0 (still TODO)
     0x06B00000  USB2 PHY    (size 0x800)   shared aw-phy

  Register layout reverse-engineered from:
    - orange-pi-5.15-sun60iw2 BSP kernel
        bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c
        bsp/drivers/usb/phy/sunxi-awphy-plat.c
    - YuzukiHD/SyterKit  include/drivers/chips/sun60iw2/reg-ccu.h
    - Live BSP register snapshots in research/sun60iw2-ccu-phy-usb-snapshot.txt

  Implementation is a clean-room reimplementation: we read the GPL
  references for the hardware-behaviour spec (register offsets and
  bit positions are facts about hardware, not copyrightable) and
  write fresh code in EDK2 style.

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

//
// CCU - Allwinner A733 (sun60iw2) Clock Control Unit
//
#define A733_CCU_BASE                0x02002000ULL

//
// PCK-600 power-domain controller (CoreLink PCK600).
// USB2 PHY block lives in power domain SUN60IW2_PCK_USB2 (id = 8).
// The PD is OFF at cold boot until somebody turns it on - if we don't,
// every write to the USB2 PHY MMIO @ 0x06B00000 is silently dropped
// (reads return 0). Found via:
//   bsp/drivers/pm_domain/pck600_domains.c
//     BASE(id) = id << 12, pwr_offset=0x0, status_offset=0x8
//     COMMAND_ON=0x8, STATUS_ON=0x8, status_mask=0xf
//   bsp/include/dt-bindings/power/sun60iw2-power.h:13
//     #define SUN60IW2_PCK_USB2  8
//   bsp/configs/linux-5.15/sun60iw2p1.dtsi:745
//     pck: pck-600@7060000 { reg = <0 0x07060000 0 0xB000>; ... }
// Live status (Linux): 0x07068000 = 0x08, 0x07068008 = 0x08.
//
#define A733_PCK600_BASE             0x07060000ULL
#define PCK600_USB2_DOMAIN_ID        8
#define PCK600_DOMAIN_BASE(id)       ((UINTN)(id) << 12)
#define PCK600_PWR_OFFSET            0x0
#define PCK600_STATUS_OFFSET         0x8
#define PCK600_COMMAND_ON            0x8
#define PCK600_STATUS_ON             0x8
#define PCK600_STATUS_MASK           0xF

//
// R_CCU (always-on / "RTC" CCU) - powers the PCK600 itself.
// CLK_R_PPU is the bus clock for the PCK600 power-domain controller.
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-r.c:144
//     SUNXI_CCU_GATE(r_ppu_clk, "r-ppu", "dcxo", 0x01AC, BIT(0), ...);
//   bsp/configs/linux-5.15/sun60iw2p1.dtsi:937
//     r_ccu: r_ccu@7010000 { reg = <0 0x07010000 0 0x340>; }
//
#define A733_R_CCU_BASE              0x07010000ULL
#define R_CCU_R_PPU_GATE_REG         0x01AC
#define R_CCU_R_PPU_GATE_BIT         BIT0

// AHB master gate register. usb-sys-ahb-gate is BIT(9), and the
// register requires AHB_MASTER_KEY (0x10000FF) in its low bits to
// accept writes. The captured live value 0xB10103F8 is what BSP Linux
// runs with - we just write it through.
#define CCU_AHB_GATE_REG             0x05C0
#define CCU_AHB_GATE_LIVE_VALUE      0xB10103F8

// Additional MBUS / msi-lite / usb-sys-ahb gate registers - confirmed
// from live Linux dump of CCU @ 0x02002000:
//   +0x05E0 = 0xF0050803  (mbus / msi-lite gates incl. usb-sys-ahb-gate)
//   +0x05E4 = 0x00000805  (companion gate set)
// Without these, the EHCI controller's AHB DMA path is gated off:
// USBCMD.PSE/ASE never echo into USBSTS.PSS/ASS because the controller
// state machine cannot run any schedule traversal.
#define CCU_MBUS_GATE0_REG           0x05E0
#define CCU_MBUS_GATE0_LIVE_VALUE    0xF0050803
#define CCU_MBUS_GATE1_REG           0x05E4
#define CCU_MBUS_GATE1_LIVE_VALUE    0x00000805

// USB clock + reset registers (offsets confirmed against BSP
// ccu-sun60iw2.c clock and reset tables).
//
//   +0x1300 usb_clk parent gate   BIT(31)  + USB_0_PHY_RSTN   BIT(30)
//   +0x1304 usb0_device           BIT(8)   + reset BIT(24)
//           usb0_ehci             BIT(4)   + reset BIT(20)
//           usb0_ohci             BIT(0)   + reset BIT(16)
//   +0x1308 usb1_clk parent gate  BIT(31)  + USB_1_PHY_RSTN   BIT(30)
//   +0x130C usb1_ehci             BIT(4)   + reset BIT(20)
//           usb1_ohci             BIT(0)   + reset BIT(16)
//   +0x1340 usb_ref               BIT(31)
//
// We just write the BSP Linux runtime values directly - they are the
// known-good steady state.
#define CCU_USB_CLK_REG              0x1300
#define CCU_USB0_GATE_RST_REG        0x1304
#define CCU_USB1_CLK_REG             0x1308
#define CCU_USB1_GATE_RST_REG        0x130C
#define CCU_USB_REF_REG              0x1340

// USB2 (xHCI2/dwc3 @ 0x06A00000 + USB2-PHY @ 0x06B00000) bus clocks
// and reset. The PHY MMIO @ 0x06B00000 lives behind THIS controller's
// AXI/AHB clock - if these are off, all reads from 0x06B00000 return 0
// and writes are silently dropped (canary 0xDEADBEEF doesn't stick).
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c:1492
//     usb2_u2_ref_clk: reg 0x1348, gate BIT(31)
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c:1499
//     usb2_suspend_clk: reg 0x1350, gate BIT(31)
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c:1508
//     usb2_mf_clk (bus_clk): reg 0x1354, gate BIT(31)
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c:2139
//     RST_USB_2: reg 0x135C, BIT(16)  (1 = released)
#define CCU_USB2_U2_REF_REG          0x1348
#define CCU_USB2_SUSPEND_REG         0x1350
#define CCU_USB2_MF_REG              0x1354
#define CCU_USB2_RST_REG             0x135C
#define CCU_GATE_BIT                 BIT31
#define CCU_USB2_RST_BIT             BIT16

// CLK_RES_DCAP_24M - "mclk" feeding the USB2 PHY block. Without this
// gate the entire PHY MMIO region @ 0x06B00000 reads as 0, even though
// the controllers behind it appear alive. Found via:
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c:1785
//     SUNXI_CCU_GATE(res_dcap_24m_clk, "res-dcap-24m", "dcxo",
//                    0x1A00, BIT(3), 0);
//   bsp/configs/linux-5.15/sun60iw2p1.dtsi:2555
//     u2phy: phy@6b00000 { clocks = <&ccu CLK_RES_DCAP_24M>; ... }
#define CCU_RES_DCAP_REG             0x1A00
#define CCU_RES_DCAP_USB_PHY_GATE    BIT3

// CLK_SERDES_PHY_CFG + RST_BUS_SERDES — drive the Cadence combo PHY subsystem.
// Without these, the DWC3 AHB/AXI bus fabric at 0x06A00000 is gated and GCTL reads 0.
//   bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c:
//     serdes_phy_cfg_clk: reg 0x13C0, gate BIT31
//     RST_BUS_SERDES:     reg 0x13C4, BIT16
//   bsp/drivers/phy/sunxi-cadence-combophy.c::sunxi_cadence_phy_serdes_init()
#define CCU_SERDES_PHY_CFG_REG       0x13C0
#define CCU_SERDES_RST_REG           0x13C4
#define CCU_SERDES_PHY_CFG_GATE      BIT31
#define CCU_SERDES_RST_BIT           BIT16

// Cadence serdes subsystem top register at 0x06C00000.
// SUBSYS_USB3P1_BGR +0x0008: BIT17=USB3P1_ACLK_EN (AXI), BIT16=USB3P1_HCLK_EN (AHB).
// combo_usb_clk_set(true) in Linux sets these — they gate the DWC3 core bus.
//   bsp/drivers/phy/sunxi-cadence-combophy.c::combo_usb_clk_set()
#define A733_SERDES_SUBSYS_BASE      0x06C00000ULL
#define SERDES_SUBSYS_USB3P1_BGR     0x0008
#define SERDES_USB3P1_ACLK_EN        BIT17
#define SERDES_USB3P1_HCLK_EN        BIT16

#define CCU_USB_CLK_LIVE_VALUE       0xC0000000  // gate + PHY_RSTN deasserted
#define CCU_USB_GATE_RST_LIVE_VALUE  0x00110011  // ehci+ohci gates + resets deasserted
#define CCU_USB_REF_LIVE_VALUE       0x80000000  // ref clock gate

//
// USB2 PHY @ 0x06B00000 - aw-phy v2 (Allwinner)
//
// Per-port layout: stride 0x20, ports 0..3 (3 = "common").
//   PHY_CTRL[n]  = +0x10 + n*0x20    SIDDQ = BIT(3) (1 = power-down)
//   RST_CTRL[n]  = +0x28 + n*0x20    PHY_RST = BIT(0) (1 = released)
//
#define A733_USB2_PHY_BASE           0x06B00000ULL
#define USBC_REG_PHY_CTRL(n)         (0x0010 + (0x20 * (n)))
#define USBC_REG_RST_CTRL(n)         (0x0028 + (0x20 * (n)))
#define USBC_PHY_SIDDQ               BIT3
#define USBC_PHY_RST                 BIT0

//
// SYSCFG @ 0x03000000 - SUN60IW2-specific USB resistance calibration.
// Per bsp/drivers/usb/host/sunxi-hci.c::usb_phyx_res_cal() and
// sunxi-hci.h offsets:
//   RESCAL_CTRL_REG    +0x0160   CAL_EN=BIT(0), PHY_o_RES200_SEL(n)=BIT(4+n)
//   RES0_CTRL_REG      +0x0164   PHY_o_RES200_TRIM(n) = (0xFF << (8*n))
//                                PHY_o_RES200_TRIM_DEFAULT(n) = (0xC8 << 8*n)
// To "enable" calibration on a port: clear CAL_EN, set RES200_SEL[n],
// and clear that port's TRIM byte (let calibration drive it).
//
#define A733_SYSCFG_BASE             0x03000000ULL
#define SYSCFG_RESCAL_CTRL_REG       0x0160
#define SYSCFG_RES0_CTRL_REG         0x0164
#define SYSCFG_CAL_EN                BIT0
#define SYSCFG_RES200_SEL(n)         (BIT4 << (n))
#define SYSCFG_RES200_TRIM_MASK(n)   ((UINT32)0xFFu << (8U * (n)))

STATIC
VOID
SunxiUsbPowerDomainOn (
  VOID
  )
{
  UINTN   Base;
  UINT32  Status;
  UINT32  Old;
  UINTN   Tries;

  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: PD on begin\n"));

  // 1. Ensure the PCK600 controller itself is clocked (CLK_R_PPU).
  Old = MmioRead32 (A733_R_CCU_BASE + R_CCU_R_PPU_GATE_REG);
  DEBUG ((DEBUG_ERROR, "  R_CCU+0x01AC (r_ppu) was 0x%08x\n", Old));
  MmioWrite32 (
    A733_R_CCU_BASE + R_CCU_R_PPU_GATE_REG,
    Old | R_CCU_R_PPU_GATE_BIT
    );
  MicroSecondDelay (10);

  // 2. Power on USB2 domain.
  Base   = (UINTN)A733_PCK600_BASE + PCK600_DOMAIN_BASE (PCK600_USB2_DOMAIN_ID);
  Status = MmioRead32 (Base + PCK600_STATUS_OFFSET);
  DEBUG ((DEBUG_ERROR, "  PCK600 USB2 status was 0x%08x\n", Status));

  if ((Status & PCK600_STATUS_MASK) != PCK600_STATUS_ON) {
    MmioAndThenOr32 (
      Base + PCK600_PWR_OFFSET,
      ~(UINT32)PCK600_STATUS_MASK,
      PCK600_COMMAND_ON
      );

    // Poll for STATUS_ON. Linux uses a 10 ms budget.
    for (Tries = 0; Tries < 1000; Tries++) {
      Status = MmioRead32 (Base + PCK600_STATUS_OFFSET);
      if ((Status & PCK600_STATUS_MASK) == PCK600_STATUS_ON) {
        break;
      }

      MicroSecondDelay (10);
    }

    DEBUG ((
      DEBUG_ERROR,
      "  PCK600 USB2 status now 0x%08x after %u tries\n",
      Status,
      (UINT32)Tries
      ));
  } else {
    DEBUG ((DEBUG_ERROR, "  PCK600 USB2 already on\n"));
  }

  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: PD on done\n"));
}

STATIC
VOID
SunxiUsbCcuInit (
  VOID
  )
{
  UINT32  Old;

  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: CCU init begin\n"));

  // 1. AHB master gate - ensure usb-sys-ahb-gate (BIT 9) is on.
  Old = MmioRead32 (A733_CCU_BASE + CCU_AHB_GATE_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x05C0 (AHB gate) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_AHB_GATE_REG, CCU_AHB_GATE_LIVE_VALUE);

  // 1b. MBUS / msi-lite / usb-sys-ahb gates. Live Linux has these set;
  // we never wrote them in builds <=14, which is why USBCMD.PSE/ASE
  // never echoed into USBSTS.PSS/ASS - the controller's DMA fabric was
  // simply ungated. Match the live values.
  Old = MmioRead32 (A733_CCU_BASE + CCU_MBUS_GATE0_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x05E0 (mbus gate0) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_MBUS_GATE0_REG, CCU_MBUS_GATE0_LIVE_VALUE);

  Old = MmioRead32 (A733_CCU_BASE + CCU_MBUS_GATE1_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x05E4 (mbus gate1) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_MBUS_GATE1_REG, CCU_MBUS_GATE1_LIVE_VALUE);

  // 1c. CLK_MSI_LITE2 gate (BIT0) + MSI_LITE2_AHB rst (BIT16) +
  //     MSI_LITE2_MBU rst (BIT17) at CCU+0x05A4. The EHCI0/EHCI1 DT
  //     binding (sun60iw2p1.dtsi:2454) requires this clock; without
  //     it the EHCI controller's MSI/IRQ path is gated and
  //     USBSTS.PSS/ASS never echo USBCMD.PSE/ASE.
  Old = MmioRead32 (A733_CCU_BASE + 0x05A4);
  DEBUG ((DEBUG_ERROR, "  CCU+0x05A4 (msi_lite2) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + 0x05A4, Old | BIT0 | BIT16 | BIT17);
  DEBUG ((DEBUG_ERROR, "  CCU+0x05A4 (msi_lite2) now 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + 0x05A4)));

  // 2. PHY mclk gate - CLK_RES_DCAP_24M, drives the USB2 PHY MMIO.
  // Without this the entire 0x06B00000 region reads 0.
  Old = MmioRead32 (A733_CCU_BASE + CCU_RES_DCAP_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1A00 (res_dcap/phy mclk) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_RES_DCAP_REG, Old | CCU_RES_DCAP_USB_PHY_GATE);
  MicroSecondDelay (10);

  // 2. USB0 - parent clock gate + PHY reset deassert
  Old = MmioRead32 (A733_CCU_BASE + CCU_USB_CLK_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1300 (usb0_clk/phy_rst) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB_CLK_REG, CCU_USB_CLK_LIVE_VALUE);

  // 3. USB0 - EHCI/OHCI/Device clock gates + resets
  Old = MmioRead32 (A733_CCU_BASE + CCU_USB0_GATE_RST_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1304 (usb0 gate+rst) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB0_GATE_RST_REG, CCU_USB_GATE_RST_LIVE_VALUE);

  // 4. USB1 - same pair
  Old = MmioRead32 (A733_CCU_BASE + CCU_USB1_CLK_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1308 (usb1_clk/phy_rst) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB1_CLK_REG, CCU_USB_CLK_LIVE_VALUE);

  Old = MmioRead32 (A733_CCU_BASE + CCU_USB1_GATE_RST_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x130C (usb1 gate+rst) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB1_GATE_RST_REG, CCU_USB_GATE_RST_LIVE_VALUE);

  // Build #29: PHY reset is now driven per-controller in
  // SunxiUsbHciPhyEnable() - assert before SIDDQ-clear, deassert after.
  // This matches BSP usb_hcd_open_clock order:
  //   USBC_Clean_SIDDP -> reset_control_deassert(reset_phy)

  // 5. USB ref clock (24 MHz reference)
  Old = MmioRead32 (A733_CCU_BASE + CCU_USB_REF_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1340 (usb_ref) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB_REF_REG, CCU_USB_REF_LIVE_VALUE);

  // 6. USB2 (xHCI2) bus clocks + reset. The USB2 PHY MMIO @ 0x06B00000
  // sits behind this controller's bus - without these the PHY is dead.
  Old = MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1348 (usb2_u2_ref) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_U2_REF_REG, Old | CCU_GATE_BIT);
  DEBUG ((DEBUG_ERROR, "    after = 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + CCU_USB2_U2_REF_REG)));

  Old = MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1350 (usb2_suspend) was 0x%08x\n", Old));
  // SUNXI_CCU_M_WITH_MUX_GATE: M[4:0], MUX[24], GATE[31]. mux 1 = sys24M.
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_SUSPEND_REG, 0x81000000);
  DEBUG ((DEBUG_ERROR, "    after = 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + CCU_USB2_SUSPEND_REG)));

  Old = MmioRead32 (A733_CCU_BASE + CCU_USB2_MF_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x1354 (usb2_mf/bus) was 0x%08x\n", Old));
  // Match live Linux: gate enable (BIT31) only, mux=0 (sys24M).
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_MF_REG, 0x80000000);
  DEBUG ((DEBUG_ERROR, "    after = 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + CCU_USB2_MF_REG)));

  // Deassert USB2 reset.
  Old = MmioRead32 (A733_CCU_BASE + CCU_USB2_RST_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x135C (usb2_rst) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_USB2_RST_REG, Old | CCU_USB2_RST_BIT);
  DEBUG ((DEBUG_ERROR, "    after = 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + CCU_USB2_RST_REG)));

  MicroSecondDelay (50);

  // 7. Cadence serdes subsystem — enable bus clocks to DWC3.
  //
  // sunxi_cadence_phy_serdes_init() in Linux (called at serdes driver probe,
  // before any per-PHY init) does exactly these three writes. Without them,
  // the DWC3 AHB/AXI slave at 0x06A00000 is bus-gated: GCTL reads 0 and
  // writes are silently dropped (build #34 symptom).
  //
  // Order: clock gate first, then reset deassert, then subsys AHB/AXI enable.
  Old = MmioRead32 (A733_CCU_BASE + CCU_SERDES_PHY_CFG_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x13C0 (serdes_phy_cfg) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_SERDES_PHY_CFG_REG, Old | CCU_SERDES_PHY_CFG_GATE);
  DEBUG ((DEBUG_ERROR, "  CCU+0x13C0 now 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + CCU_SERDES_PHY_CFG_REG)));
  MicroSecondDelay (10);

  Old = MmioRead32 (A733_CCU_BASE + CCU_SERDES_RST_REG);
  DEBUG ((DEBUG_ERROR, "  CCU+0x13C4 (serdes_rst) was 0x%08x\n", Old));
  MmioWrite32 (A733_CCU_BASE + CCU_SERDES_RST_REG, Old | CCU_SERDES_RST_BIT);
  DEBUG ((DEBUG_ERROR, "  CCU+0x13C4 now 0x%08x\n",
    MmioRead32 (A733_CCU_BASE + CCU_SERDES_RST_REG)));
  MicroSecondDelay (50);

  Old = MmioRead32 (A733_SERDES_SUBSYS_BASE + SERDES_SUBSYS_USB3P1_BGR);
  DEBUG ((DEBUG_ERROR, "  SERDES+0x0008 (usb3p1_bgr) was 0x%08x\n", Old));
  MmioWrite32 (A733_SERDES_SUBSYS_BASE + SERDES_SUBSYS_USB3P1_BGR,
               Old | SERDES_USB3P1_ACLK_EN | SERDES_USB3P1_HCLK_EN);
  DEBUG ((DEBUG_ERROR, "  SERDES+0x0008 now 0x%08x\n",
    MmioRead32 (A733_SERDES_SUBSYS_BASE + SERDES_SUBSYS_USB3P1_BGR)));
  MicroSecondDelay (10);

  // Settle.
  MicroSecondDelay (100);
  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: CCU init done\n"));
}

STATIC
VOID
SunxiUsbResCal (
  IN UINTN  UsbcNo
  )
{
  UINT32  Val;

  // Per BSP usb_phyx_res_cal(enable=true): clear CAL_EN, set this
  // port's RES200_SEL bit, and clear its TRIM byte. SUN60IW2 uses
  // RES0_CTRL_REG for the trim (same address as RES200_CTRL_REG -
  // the BSP defines them both at 0x0164).
  Val  = MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL_REG);
  DEBUG ((
    DEBUG_ERROR,
    "  SYSCFG+0x160 (rescal) was 0x%08x\n",
    Val
    ));
  Val &= ~(UINT32)SYSCFG_CAL_EN;
  Val |=  SYSCFG_RES200_SEL (UsbcNo);
  MmioWrite32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL_REG, Val);

  Val  = MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RES0_CTRL_REG);
  DEBUG ((
    DEBUG_ERROR,
    "  SYSCFG+0x164 (res0)   was 0x%08x\n",
    Val
    ));
  Val &= ~SYSCFG_RES200_TRIM_MASK (UsbcNo);
  MmioWrite32 (A733_SYSCFG_BASE + SYSCFG_RES0_CTRL_REG, Val);

  DEBUG ((
    DEBUG_ERROR,
    "  SYSCFG+0x160 now 0x%08x  +0x164 now 0x%08x\n",
    MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL_REG),
    MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RES0_CTRL_REG)
    ));
}

//
// EHCI-internal PHY control. For sun60iw2 the EHCI MMIO contains the
// PHY power-down/SIDDQ bit at +0x810 bit 3 (per
// bsp/drivers/usb/host/sunxi-hci.h:106 SUNXI_HCI_PHY_CTRL=0x810
// and :127 SUNXI_HCI_PHY_CTRL_SIDDQ=3 for SUN60IW2).
// Also clear SUNXI_HCI_CTRL_3 bit 9 (FORCE_SUSPEND) at +0x808.
// Enable PMU IRQ at +0x800.
//
#define HCI_USB_CTRL_REG             0x0800
#define HCI_CTRL_3_REG               0x0808
#define HCI_PHY_CTRL_REG             0x0810
#define HCI_PHY_TUNE_REG             0x0818
#define HCI_PHY_CTRL_SIDDQ_BIT       BIT3
#define HCI_CTRL_3_FORCE_SUSPEND_BIT BIT9
//
// PHY tuning value - read from live, working Linux register state on
// this exact A733 board. Without it the EHCI UTMI PHY signal levels
// are wrong and the controller stays halted.
//
#define HCI_PHY_TUNE_VALUE  0x063338C6

//
// SUN60IW2 EHCI "passby" / AHB master configuration. Per BSP
// bsp/drivers/usb/host/sunxi-hci.c::__usb_passby() these bits in
// SUNXI_USB_PMU_IRQ_ENABLE (+0x800) are required for EHCI transfer
// descriptors to actually be fetched over AHB:
//   BIT15 - bypass OHCI bulk out changes (sun60iw2-specific)
//   BIT11 - AHB Master interface INCR16 enable (sun60iw2-specific)
//   BIT9  - AHB Master interface burst type INCR4 enable
//   BIT8  - AHB Master interface INCRX align enable
//   BIT0  - For HCI0: enable UTMI, disable ULPI; for HCI1: ULPI bypass
//           (we set BIT0 unconditionally - both meanings select the
//           on-chip USB2 PHY for the controller).
//
#define HCI_PASSBY_BITS  (BIT15 | BIT11 | BIT9 | BIT8 | BIT0)

STATIC
VOID
SunxiUsbHciPhyEnable (
  IN UINTN  HciBase,
  IN UINTN  CcuPhyClkReg
  )
{
  UINT32  Val;

  // Build #29: BSP order is
  //   USBC_Clean_SIDDP()                   // clear HCI+0x810 bit3
  //   reset_control_deassert(reset_phy)    // CCU bit30
  // We previously deasserted phy_rst in CCU init (before SIDDQ was
  // cleared). UTMI_PHY_STATUS (+0x824) stayed 0 -> PHY clock did not
  // come up. Try: assert phy_rst here, clear SIDDQ, then deassert.
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x: assert phy_rst on CCU+0x%x\n",
    (UINT32)HciBase, (UINT32)CcuPhyClkReg));
  MmioWrite32 (A733_CCU_BASE + CcuPhyClkReg,
    CCU_USB_CLK_LIVE_VALUE & ~BIT30);
  MicroSecondDelay (200);

  // Clear FORCE_SUSPEND so the controller doesn't keep the PHY parked.
  Val = MmioRead32 (HciBase + HCI_CTRL_3_REG);
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x +0x808 was 0x%08x\n",
    (UINT32)HciBase, Val));
  Val &= ~(UINT32)HCI_CTRL_3_FORCE_SUSPEND_BIT;
  MmioWrite32 (HciBase + HCI_CTRL_3_REG, Val);

  // Power up PHY: clear SIDDQ.
  Val = MmioRead32 (HciBase + HCI_PHY_CTRL_REG);
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x +0x810 was 0x%08x\n",
    (UINT32)HciBase, Val));
  Val &= ~(UINT32)HCI_PHY_CTRL_SIDDQ_BIT;
  MmioWrite32 (HciBase + HCI_PHY_CTRL_REG, Val);

  // EHCI passby / AHB master config.
  Val = MmioRead32 (HciBase + HCI_USB_CTRL_REG);
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x +0x800 was 0x%08x\n",
    (UINT32)HciBase, Val));
  Val |= HCI_PASSBY_BITS;
  MmioWrite32 (HciBase + HCI_USB_CTRL_REG, Val);

  // PHY tune - signal-level / driver-strength register. The BSP reads
  // the value from `aw,phy_tune_param` in DT and writes it via
  // usb_hci_utmi_phy_tune. We hard-code the value observed live.
  Val = MmioRead32 (HciBase + HCI_PHY_TUNE_REG);
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x +0x818 was 0x%08x\n",
    (UINT32)HciBase, Val));
  MmioWrite32 (HciBase + HCI_PHY_TUNE_REG, HCI_PHY_TUNE_VALUE);

  // Build #28: undocumented HCI register +0x81C. Live Linux has 0x53
  // (bits 0,1,4,6 - likely UTMI bus/clock enables). EDK2 reads 0 here
  // and PSS never echoes - try writing the live value.
  Val = MmioRead32 (HciBase + 0x81C);
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x +0x81C was 0x%08x, writing 0x53\n",
    (UINT32)HciBase, Val));
  MmioWrite32 (HciBase + 0x81C, 0x00000053);

  // Now (after SIDDQ cleared, PMU/passby set, tune written) deassert
  // the PHY reset. PHY clock domain should now come alive.
  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x: deassert phy_rst on CCU+0x%x\n",
    (UINT32)HciBase, (UINT32)CcuPhyClkReg));
  MmioWrite32 (A733_CCU_BASE + CcuPhyClkReg, CCU_USB_CLK_LIVE_VALUE);
  MicroSecondDelay (1000);

  DEBUG ((DEBUG_ERROR, "  HCI@0x%08x +0x81C now 0x%08x  +0x824 (UTMI_STAT)=0x%08x\n",
    (UINT32)HciBase,
    MmioRead32 (HciBase + 0x81C),
    MmioRead32 (HciBase + 0x824)));

  DEBUG ((DEBUG_ERROR,
    "  HCI@0x%08x +0x800=0x%08x +0x808=0x%08x +0x810=0x%08x +0x818=0x%08x\n",
    (UINT32)HciBase,
    MmioRead32 (HciBase + HCI_USB_CTRL_REG),
    MmioRead32 (HciBase + HCI_CTRL_3_REG),
    MmioRead32 (HciBase + HCI_PHY_CTRL_REG),
    MmioRead32 (HciBase + HCI_PHY_TUNE_REG)));
}

STATIC
VOID
SunxiUsbPhyInit (
  VOID
  )
{
  UINT32 Val;

  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: PHY init begin\n"));

  // SYSCFG resistance calibration for ports 0 and 1.
  SunxiUsbResCal (0);
  SunxiUsbResCal (1);

  MicroSecondDelay (10);

  // SelectPhyToHci: route the OTG-shared PHY0 to EHCI0/OHCI0 host.
  //   bsp/drivers/usb/host/sunxi-hci.c::USBC_SelectPhyToHci()
  // The BSP does exactly this for HCI0_USBC_NO:
  //     reg = readl(otg_vbase + 0x420);
  //     reg &= ~0x01;
  //     writel(reg, otg_vbase + 0x420);
  // Live Linux ends up with 0x40000000 (BIT30 set, BIT0 cleared); BIT30
  // is set by some other init path (likely MUSB OTG bringup), not by
  // SelectPhyToHci itself. We follow the BSP and only clear BIT0.
  //
  // BUILD #31: must happen *before* EHCI0's PHY reset/SIDDQ sequence,
  // otherwise the OTG block is still owning PHY0 when we try to clear
  // SIDDQ and the UTMI clock never starts on EHCI0.
  Val = MmioRead32 (0x04100000ULL + 0x420);
  DEBUG ((DEBUG_ERROR, "  OTG+0x420 (phy_cfg) was 0x%08x\n", Val));
  Val &= ~(UINT32)BIT0;     // route PHY0 to EHCI host (release from OTG)
  MmioWrite32 (0x04100000ULL + 0x420, Val);
  DEBUG ((DEBUG_ERROR, "  OTG+0x420 (phy_cfg) now 0x%08x\n",
    MmioRead32 (0x04100000ULL + 0x420)));

  // EHCI0 PHY: clear SIDDQ at 0x04101810 bit 3, then deassert PHY reset
  // at CCU+0x1300 BIT(30).
  SunxiUsbHciPhyEnable (0x04101000, CCU_USB_CLK_REG);
  // EHCI1 PHY: clear SIDDQ at 0x04200810 bit 3, then deassert PHY reset
  // at CCU+0x1308 BIT(30).
  SunxiUsbHciPhyEnable (0x04200000, CCU_USB1_CLK_REG);

  // === BUILD #20: Initialize the USB2 PHY core at 0x06B00000 directly ===
  //
  // The BSP driver `bsp/drivers/usb/dwc3/phy-sunxi-plat.c` matches
  // `allwinner,sunxi-plat-phy` and on probe does:
  //   1. clk_prepare_enable(CLK_RES_DCAP_24M)        [we already do this]
  //   2. phy_res_set(true) -> phy_rescal_set_v2:
  //        SYSCFG+0x160: clear CAL_EN(BIT0); set PCIE_USB_RES200_TRIM_SEL(BIT10)
  //   3. phy_u2_set(true): clear SIDDQ at PHY+0x10
  //   4. writel(phy->param=0x143338D6, PHY+0x18)
  //
  // Live Linux register dump (Round-2 captured):
  //   SYSCFG+0x160 = 0x00C81532 (we currently write 0x00C81132 - bit10 missing)
  //   PHY+0x10     = 0x000E2430 (we never write this - reads 0)
  //   PHY+0x18     = 0x143338D6 (we never write this - reads 0)
  //   PHY+0x30     = 0x000E2430 (port 1)
  //   PHY+0x38     = 0x143338D6 (port 1)
  //
  // Without this block the PHY does not produce a UTMI clock to the EHCI,
  // so PSE-set never sees PSS-echo and EhciDxe times out.
  Val = MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL_REG);
  DEBUG ((DEBUG_ERROR, "  SYSCFG+0x160 (pre-bit10) = 0x%08x\n", Val));
  Val |= BIT10;                                     // PCIE_USB_RES200_TRIM_SEL
  MmioWrite32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL_REG, Val);
  DEBUG ((DEBUG_ERROR, "  SYSCFG+0x160 (post-bit10) = 0x%08x\n",
    MmioRead32 (A733_SYSCFG_BASE + SYSCFG_RESCAL_CTRL_REG)));

  // Sanity: confirm PHY MMIO is writable. Read pre-write, write live value,
  // read post-write - if the readback != written value the block is gated.
  DEBUG ((DEBUG_ERROR,
    "  PHY@0x06B00000 PRE  +0x10=0x%08x +0x18=0x%08x +0x30=0x%08x +0x38=0x%08x\n",
    MmioRead32 (A733_USB2_PHY_BASE + 0x10),
    MmioRead32 (A733_USB2_PHY_BASE + 0x18),
    MmioRead32 (A733_USB2_PHY_BASE + 0x30),
    MmioRead32 (A733_USB2_PHY_BASE + 0x38)));

  // Port 0
  MmioWrite32 (A733_USB2_PHY_BASE + 0x10, 0x000E2430);
  MmioWrite32 (A733_USB2_PHY_BASE + 0x18, 0x143338D6);
  // Port 1
  MmioWrite32 (A733_USB2_PHY_BASE + 0x30, 0x000E2430);
  MmioWrite32 (A733_USB2_PHY_BASE + 0x38, 0x143338D6);

  MicroSecondDelay (10);

  DEBUG ((DEBUG_ERROR,
    "  PHY@0x06B00000 POST +0x10=0x%08x +0x18=0x%08x +0x30=0x%08x +0x38=0x%08x\n",
    MmioRead32 (A733_USB2_PHY_BASE + 0x10),
    MmioRead32 (A733_USB2_PHY_BASE + 0x18),
    MmioRead32 (A733_USB2_PHY_BASE + 0x30),
    MmioRead32 (A733_USB2_PHY_BASE + 0x38)));

  // VBUS for EHCI1 / OHCI1 - reg_usb1_vbus is wired to PB7
  // ACTIVE_LOW with regulator-always-on,boot-on. We must drive PB7
  // LOW so VBUS is enabled. PB7 also needs to be a GPIO output.
  //
  // sun60iw2 PIO layout (HW_TYPE_4 in BSP pinctrl-sunxi.c):
  //   initial_bank_offset = 0x80
  //   bank_mem_size       = 0x80
  //   mux_regs_offset     = 0x00 (4 bits per pin, 8 pins per reg)
  //   data_regs_offset    = 0x10
  //   bank_base[0] -> PB (since first entry = SUNXI_BANK_OFFSET('B','A'))
  //
  // PB lives at 0x02000080. PB_MUX0 (pins 0..7) at +0x80+0x00 = 0x02000080,
  // PB_DAT at +0x80+0x10 = 0x02000090.
  // PB7 mux: bits[31:28]; PB7 data: bit 7.
  //
  Val = MmioRead32 (0x02000080ULL);
  DEBUG ((DEBUG_ERROR, "  PIO PB_MUX0 was 0x%08x\n", Val));
  Val &= ~((UINT32)0xF << 28);
  Val |=  ((UINT32)0x1 << 28);   // PB7 = output
  MmioWrite32 (0x02000080ULL, Val);

  Val = MmioRead32 (0x02000090ULL);
  DEBUG ((DEBUG_ERROR, "  PIO PB_DAT  was 0x%08x\n", Val));
  Val &= ~(UINT32)BIT7;          // drive LOW -> enable VBUS1 (active-low)
  MmioWrite32 (0x02000090ULL, Val);

  DEBUG ((DEBUG_ERROR, "  PIO PB_MUX0 now 0x%08x  PB_DAT now 0x%08x\n",
    MmioRead32 (0x02000080ULL), MmioRead32 (0x02000090ULL)));

  MicroSecondDelay (5000);       // wait for VBUS rise + device debounce

  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: PHY init done\n"));
}

EFI_STATUS
EFIAPI
SunxiUsbDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Cap;

  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: entry\n"));

  // ENTRY-time PHY MMIO sanity check: read PHY @ 0x06B00000 BEFORE we
  // touch CCU/PCK600/SYSCFG. This tells us whether U-Boot left the PHY
  // accessible (we'd then know our own init clobbers it) or whether it
  // was already gated coming out of U-Boot.
  DEBUG ((DEBUG_ERROR,
    "  ENTRY PHY @0x06B00000 +0x10=0x%08x +0x18=0x%08x +0x1A00(CCU)=0x%08x\n",
    MmioRead32 (0x06B00000ULL + 0x10),
    MmioRead32 (0x06B00000ULL + 0x18),
    MmioRead32 (0x02002000ULL + 0x1A00)));

  // ENTRY-time write probe: try writing a sentinel to PHY+0x10 and
  // reading back. If write fails here too, the block is gated by
  // something U-Boot didn't enable. If write succeeds here but our
  // later writes fail, our CCU init disabled access.
  MmioWrite32 (0x06B00000ULL + 0x10, 0xDEADBEEF);
  DEBUG ((DEBUG_ERROR,
    "  ENTRY PHY write-probe: wrote 0xDEADBEEF, read back 0x%08x\n",
    MmioRead32 (0x06B00000ULL + 0x10)));

  //
  // Bring up clocks/resets/PHY before exposing the controllers, so
  // EhciDxe sees a powered, out-of-reset device when it probes MMIO.
  //
  SunxiUsbPowerDomainOn ();
  SunxiUsbCcuInit ();
  SunxiUsbPhyInit ();

  // Sanity probe - dump capability + operational regs for both EHCIs.
  for (UINTN Idx = 0; Idx < 2; Idx++) {
    UINTN Base = (Idx == 0) ? 0x04101000ULL : 0x04200000ULL;
    UINT32 CapL;
    Cap = MmioRead32 (Base + 0x00);  // HCCAPBASE
    CapL = Cap & 0xFF;               // CAPLENGTH
    DEBUG ((DEBUG_ERROR,
      "SunxiUsbDxe: EHCI%u HCCAPBASE=0x%08x HCSPARAMS=0x%08x HCCPARAMS=0x%08x\n",
      (UINT32)Idx, Cap,
      MmioRead32 (Base + 0x04),
      MmioRead32 (Base + 0x08)));
    DEBUG ((DEBUG_ERROR,
      "SunxiUsbDxe: EHCI%u USBCMD=0x%08x USBSTS=0x%08x USBINTR=0x%08x CONFIGFLAG=0x%08x\n",
      (UINT32)Idx,
      MmioRead32 (Base + CapL + 0x00),
      MmioRead32 (Base + CapL + 0x04),
      MmioRead32 (Base + CapL + 0x08),
      MmioRead32 (Base + CapL + 0x40)));
    DEBUG ((DEBUG_ERROR,
      "SunxiUsbDxe: EHCI%u PORTSC0=0x%08x\n",
      (UINT32)Idx,
      MmioRead32 (Base + CapL + 0x44)));
  }

  // xHCI2 (USB 3.0 controller, snps,dwc3 at 0x06A00000). Build #34:
  // Live PCK snapshot showed SUN60IW2_PCK_USB2 power domain (PCK+0x8000)
  // already on at boot - so it isn't the missing piece. What we never
  // did is touch the DWC3 core itself. In Linux dwc3_core_init() does a
  // PHYSOFTRST + GCTL.CORESOFTRESET toggle, and dwc3_set_prtcap() sets
  // PRTCAPDIR=HOST in GCTL. Without that, the xHCI register window
  // exposed by the DWC3 wrapper stays as zeros (CAPLENGTH=0).
  //
  // Probe-then-init-then-probe so we can see what the CCU init alone
  // gave us versus what the DWC3 reset/PRTCAP unlocks.
  {
    UINT32  Cap0, Cap1, GCtl;

    Cap0 = MmioRead32 (0x06A00000);
    DEBUG ((DEBUG_ERROR, "  xHCI2 pre-DWC3-init: +0x0000=0x%08x\n", Cap0));

    // Assert PHY soft reset on both USB2 and USB3 PHY-cfg blocks.
    MmioOr32  (0x06A00000 + 0xC200, BIT31);  // GUSB2PHYCFG0.PHYSOFTRST
    MmioOr32  (0x06A00000 + 0xC2C0, BIT31);  // GUSB3PIPECTL0.PHYSOFTRST
    MicroSecondDelay (200);
    MmioAnd32 (0x06A00000 + 0xC200, ~(UINT32)BIT31);
    MmioAnd32 (0x06A00000 + 0xC2C0, ~(UINT32)BIT31);
    MicroSecondDelay (200);

    // Toggle DWC3 core soft reset (GCTL bit 11).
    GCtl = MmioRead32 (0x06A00000 + 0xC100);
    DEBUG ((DEBUG_ERROR, "  DWC3 GCTL was 0x%08x\n", GCtl));
    MmioOr32  (0x06A00000 + 0xC100, BIT11);
    MicroSecondDelay (50);
    MmioAnd32 (0x06A00000 + 0xC100, ~(UINT32)BIT11);
    MicroSecondDelay (50);

    // Set PRTCAPDIR = HOST (1) in GCTL[13:12]. Clear, then set bit 12.
    GCtl = MmioRead32 (0x06A00000 + 0xC100);
    GCtl &= ~(UINT32)(BIT12 | BIT13);
    GCtl |=  BIT12;
    MmioWrite32 (0x06A00000 + 0xC100, GCtl);
    DEBUG ((DEBUG_ERROR, "  DWC3 GCTL now 0x%08x\n",
      MmioRead32 (0x06A00000 + 0xC100)));

    Cap1 = MmioRead32 (0x06A00000);
    DEBUG ((DEBUG_ERROR, "  xHCI2 post-DWC3-init: +0x0000=0x%08x\n", Cap1));
  }

  // Build #42: xHCI registered and DWC3 GCTL confirmed live (0x1→0x1001).
  // Build #43: XhciDxe attaches and hangs on HCRESET — USB3 PIPE (Cadence
  // combo0_usb PHY) is uninitialized so the USB3 port state-machine never
  // halts. Suppress registration until Cadence PHY init is implemented.
  // Keep the DWC3 CCU/serdes init above so the GCTL diag prints remain.
#if 0
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL, NULL, 1,
             0x06A00000ULL, 0x00100000ULL
             );
  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: xHCI2 register: %r\n", Status));
#endif

  // EHCI0 - left-bottom USB-A port. Build #31 fixed the
  // OTG-PHY-routing order (OTG+0x420 &= ~BIT0 now happens before the
  // EHCI0 SIDDQ-clear / PHY-reset sequence). Re-enable registration.
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL, NULL, 1,
             0x04101000ULL, 0x00000400ULL
             );
  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: EHCI0 register: %r\n", Status));


  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL, NULL, 1,
             0x04200000ULL, 0x00000400ULL
             );
  DEBUG ((DEBUG_ERROR, "SunxiUsbDxe: EHCI1 register: %r\n", Status));

  // NOTE: EDK2 has no OhciDxe in MdeModulePkg, so we cannot register
  // OHCI companions even though they exist on the board (full-speed
  // and low-speed devices like keyboards land here in Linux).
  // The Razer keyboard on this board is a full-speed device on
  // OHCI0 - it will not enumerate under UEFI without an OHCI driver.

  return EFI_SUCCESS;
}
