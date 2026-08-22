/** @file
  SunxiPcieDxe - bring the Allwinner A733 (sun60iw2) DesignWare PCIe root
  complex up from cold.

  This driver replaces an earlier one that assumed BSP U-Boot had already
  trained the link. That assumption was false: the vendor U-Boot on this board
  is built with CONFIG_PCI unset and CONFIG_AW_CADENCE_COMBOPHY unset, so it
  never touches PCIe at all. The old driver read the DBI window before any
  clock was running and hung with no output, which was misread at the time as
  a "locked" DBI. The block was not locked, it was unclocked.

  Bring-up order, which matters:

    1. power GPIO PL3 high
    2. hold PERST# (PD22) low
    3. CCU: serdes reset+clock, PCIe resets, aux clock, AXI slave, ITS
    4. dcxo serdes1 gate, serdes subsystem, and the combo1 PCIe/USB3 mux
    5. the Cadence combo PHY register sequence, ending on a PMA-ready poll
    6. release PERST#
    7. enable LTSSM and wait for SMLH + RDLH
    8. only now is it safe to touch DBI

  Nothing before step 3 may read DBI or the app block. Every step prints
  before it runs, so a hang is attributable from the UART log alone.

  Register facts were taken from the board's own device tree and from the
  Allwinner CCU driver's register tables, then checked against live hardware
  with the vendor kernel running (app+0xC00 read 0x41 = LINK_TRAINING |
  DEVICE_TYPE_RC, app+0xE0C read 0x13 = SMLH | RDLH). See
  research/a733-pcie-nvme-bringup.md for the measurements and their sources.

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

//
// Clock control unit.
//
#define A733_CCU_BASE                 0x02002000ULL
#define CCU_ITS_BGR                   0x0574        // RST_BUS_ITS_PCIE0 / CLK_ITS_PCIE0_A
#define   ITS_PCIE0_RST               BIT16
#define   ITS_PCIE0_GATE              BIT1
#define CCU_PCIE_AUX_CLK              0x1380        // CLK_PCIE0_AUX
#define   PCIE_AUX_GATE               BIT31
#define CCU_PCIE_AXI_SLV_CLK          0x1384        // CLK_PCIE0_AXI_SLV
#define   PCIE_AXI_SLV_GATE           BIT31
#define   PCIE_AXI_SLV_SRC_400M       (2U << 24)
#define CCU_PCIE_BGR                  0x138C
#define   PCIE_RST                    BIT17         // RST_BUS_PCIE0
#define   PCIE_PWRUP_RST              BIT16         // RST_BUS_PCIE0_PWRUP
#define CCU_SERDES_PHY_CFG_CLK        0x13C0        // CLK_SERDES_PHY_CFG
#define   SERDES_PHY_CFG_GATE         BIT31
//
// SUNXI_CCU_M_WITH_MUX_GATE: M divider in bits 0-4, parent mux in bits 24-26,
// gate at bit 31. Parents are { sys24M, pll-peri0-600m }. The vendor sets this
// clock to 100 MHz, so pick the 600 MHz parent and divide by 6. Setting only
// the gate leaves it on the 24 MHz reset default.
//
#define   SERDES_PHY_CFG_M_MASK       0x0000001FU
#define   SERDES_PHY_CFG_MUX_MASK     (0x7U << 24)
#define   SERDES_PHY_CFG_MUX_PERI600  (0x1U << 24)
#define   SERDES_PHY_CFG_M_DIV6       (6U - 1U)
#define CCU_SERDES_BGR                0x13C4
#define   SERDES_RST                  BIT16         // RST_BUS_SERDES

//
// The lone serdes register out in the PRCM area, third entry of the serdes
// node's reg list. BIT5 is the serdes1 dcxo gate.
//
#define A733_DCXO_SERDES_REG          0x0709016CULL
#define   DCXO_SERDES1_GATING         BIT5

//
// serdes@6c00000 and its combo-phy1 child, per the device tree.
//
#define SERDES_SUBSYS_BASE            0x06C00000ULL
#define   SUBSYS_PCIE_BGR             0x004
#define     SUBSYS_PCIE_GATING        (BIT16 | BIT17 | BIT18)
#define   SUBSYS_DBG_CTL              0x0F0
#define     SUBSYS_DIS_COMBO1_AUTOGATE BIT29
#define SERDES_COMBO_BASE             0x06C06000ULL
#define   SUBSYS_COMB1_PIPE           0xC44
#define     SUBSYS_COMB1_PIPE_PCIE    0x1
#define COMBO1_TOP_BASE               0x06C02000ULL
#define COMBO1_PHY_BASE               0x06CA0000ULL

//
// Pin controllers. The main PIO keeps a table of bank offsets at offset 0,
// so bank N's base is read from there rather than computed: the strides are
// not uniform (PA is at 0x000 but PB is at 0x100).
//
#define A733_PIO_BASE                 0x02000000ULL
#define A733_R_PIO_BASE               0x07025000ULL   // bank PL lives at offset 0
#define PIO_CFG0                      0x00
#define PIO_DATA                      0x10
#define PIO_MUX_OUTPUT                0x1

#define PIO_BANK_D                    3               // PERST# and WAKE
#define PIN_PERST                     22              // PD22, active high = released
#define PIN_WAKE                      21              // PD21
#define PIN_POWER                     3               // PL3 on R_PIO, active high

//
// PCIe root complex. The app block sits inside the DBI window, which is why
// the device tree declares only one reg range.
//
#define A733_PCIE_DBI_BASE            0x06000000ULL
#define A733_PCIE_APP_BASE            (A733_PCIE_DBI_BASE + 0x400000)
#define   APP_LTSSM_CTRL              0xC00
#define     APP_LINK_TRAINING         BIT0
#define     APP_DEVICE_TYPE_RC        BIT6
#define   APP_PHY_CFG                 0x800   // golden value under Linux: 0x00a023f0
//
// Reading this register on the running board with the link up under Linux gives
// 0x00a023f0, while ours settles at 0x008023f0. The only difference is bit 21.
// The vendor U-Boot never writes this register, so the Linux BSP sets it, and
// the header lists SYS_CLK / PAD_CLK immediately below PCIE_PHY_CFG, which
// suggests a reference clock source select. A wrong refclk fits the symptom
// exactly: the physical layer trains but the data link never completes.
//
#define     APP_PHY_CFG_BIT21           BIT21
#define   APP_LINK_STAT               0xE0C
#define     APP_SMLH_LINK_UP          BIT0
#define     APP_RDLH_LINK_UP          BIT1
#define   APP_LINK_UP                 (APP_SMLH_LINK_UP | APP_RDLH_LINK_UP)

//
// DesignWare link configuration, in DBI. The core has to be told the lane
// count and the target speed BEFORE link training is enabled; leaving it at
// reset defaults gets the physical layer up (SMLH) but the data link layer
// never completes (RDLH stays 0), which is exactly what the first hardware
// run showed.
//
#define   DBI_PORT_LINK_CONTROL           0x710
#define     PORT_LINK_MODE_MASK           (0x3FU << 16)
#define     PORT_LINK_MODE_1_LANE         (0x01U << 16)
#define   DBI_LINK_WIDTH_SPEED_CTRL       0x80C
#define     PORT_LOGIC_LINK_WIDTH_MASK    (0x1FFU << 8)
#define     PORT_LOGIC_LINK_WIDTH_1_LANE  (0x001U << 8)
#define   DBI_MISC_CONTROL_1_CFG          0x8BC
#define     DBI_RO_WR_EN                  BIT0
#define   DBI_CAP_LIST_PTR                0x34
#define     PCI_CAP_ID_EXP                0x10
#define     PCI_CAP_ID_MAX                0x14
#define     CAP_ID_MASK                   0x00FF
#define     NEXT_CAP_PTR_MASK             0xFF00
#define     PCI_EXP_LNKCAP                12      // cap + 0x0C
#define       PCI_EXP_LNKCAP_SLS          0x0000000FU
#define     PCI_EXP_LNKCTL2               48      // cap + 0x30
#define       PCI_EXP_LNKCTL2_TLS         0x0000000FU
#define     PCIE_LINK_SPEED_GEN3          0x3

//
// Root complex config, from the vendor setup_rc(). Not needed to train the
// link, but needed before anything can enumerate behind it.
//
#define   DBI_BAR0                        0x10
#define   DBI_BAR1                        0x14
#define   DBI_PRIMARY_BUS                 0x18
#define     RC_BUS_NUMBERS                0x00FF0100U   // pri 0, sec 1, sub 0xff
#define   DBI_COMMAND                     0x04
#define     CMD_IO                        BIT0
#define     CMD_MEMORY                    BIT1
#define     CMD_MASTER                    BIT2
#define     CMD_SERR                      BIT8

//
// Timeouts. The vendor code spins forever on the PMA-ready poll; firmware
// must not, so every wait here is bounded and reports where it gave up.
//
#define PHY_POLL_INTERVAL_US          100
#define PHY_POLL_TIMEOUT_US           100000        // 100 ms
#define LINK_POLL_INTERVAL_US         1000
#define LINK_POLL_TIMEOUT_US          1000000       // 1 s, generous for gen3 retrain

//
// One entry of the Cadence combo PHY init sequence. The table below is
// generated from the vendor sequence by scratchpad/genphy.py rather than
// transcribed, because a single mistyped offset here would be extremely hard
// to diagnose on hardware.
//
typedef enum {
  A733_PHY_TGT_TOP = 0,     // combo1 top registers   @ COMBO1_TOP_BASE
  A733_PHY_TGT_PHY = 1      // combo1 PHY array       @ COMBO1_PHY_BASE
} A733_PHY_TARGET;

typedef enum {
  A733_PHY_OP_WRITE16 = 0,
  A733_PHY_OP_WRITE32 = 1,
  A733_PHY_OP_ANDOR16 = 2,
  A733_PHY_OP_ANDOR32 = 3,
  A733_PHY_OP_POLL32  = 4   // wait until (read32 & Value) != 0
} A733_PHY_OPCODE;

typedef struct {
  UINT8     Target;
  UINT8     Op;
  UINT32    Offset;
  UINT32    Value;          // value, OR mask, or poll mask
  UINT32    Mask;           // AND mask for the ANDOR forms
} A733_PHY_OP;

STATIC CONST A733_PHY_OP  mPciePhyInitOps[] = {
  { A733_PHY_TGT_TOP  , A733_PHY_OP_WRITE32   , 0x00004, 0x01100001, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00044, 0x0000001A, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00054, 0x00000034, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00058, 0x000000DA, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00064, 0x00000034, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00068, 0x000000DA, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x000C8, 0x00000082, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x000CA, 0x00000082, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x000E8, 0x0000001A, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00208, 0x00000020, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0020A, 0x00000007, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00218, 0x00000020, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0021A, 0x00000007, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00228, 0x0000030C, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0022A, 0x00000007, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00248, 0x00000007, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0024A, 0x00000003, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0024C, 0x0000000F, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00250, 0x00000132, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x08180, 0x00000208, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x08184, 0x0000009C, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x10088, 0x0000001A, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x1008A, 0x00000082, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x10098, 0x0000001A, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x1009A, 0x00000082, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x08246, 0x00000A28, 0x00000000 },
  { A733_PHY_TGT_TOP  , A733_PHY_OP_ANDOR32   , 0x00100, 0x00000000, 0xFFFFFFCF },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00128, 0x00000004, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00148, 0x00000004, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001A8, 0x00000004, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00348, 0x00000509, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00368, 0x00000509, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00388, 0x00000509, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0034A, 0x00000F00, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0036A, 0x00000F00, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0038A, 0x00000F00, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0034C, 0x00000F08, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0036C, 0x00000F08, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0038C, 0x00000F08, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00120, 0x00000180, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00140, 0x00000133, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001A0, 0x00000133, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00122, 0x00009D8A, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00142, 0x0000B13B, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001A2, 0x0000B13B, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00124, 0x00000002, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00144, 0x00000002, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001A4, 0x00000002, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00126, 0x00000102, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00146, 0x000000CE, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001A6, 0x000000CE, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00340, 0x00000022, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00360, 0x00000022, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00380, 0x00000022, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00130, 0x00000001, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00150, 0x00000001, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001B0, 0x00000001, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00132, 0x0000045F, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00152, 0x000002F0, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001B2, 0x00000399, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00134, 0x0000006B, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00154, 0x00000068, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001B4, 0x00000068, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00136, 0x00000004, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00156, 0x00000004, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001B6, 0x00000004, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00108, 0x00000104, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00188, 0x00000104, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0010A, 0x00000005, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0018A, 0x00000005, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0010C, 0x00000337, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0018C, 0x00000337, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00110, 0x00003DBE, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00190, 0x00003DBE, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00104, 0x00000003, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00184, 0x00000003, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x00138, 0x00000014, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001B8, 0x00000014, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0013C, 0x00000192, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001BC, 0x00000192, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x0013E, 0x00000006, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x001BE, 0x00000006, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x103C0, 0x00000000, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x102E2, 0x00000019, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x102E4, 0x00000019, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x103FE, 0x00000001, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_ANDOR32   , 0x00098, 0x00000002, 0xFFFFFFFC },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_ANDOR32   , 0x000A8, 0x00000002, 0xFFFFFFFC },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_ANDOR32   , 0x000D8, 0x00000002, 0xFFFFFFFC },
  { A733_PHY_TGT_TOP  , A733_PHY_OP_ANDOR32   , 0x00000, 0x00000001, 0xFFFFFFFF },
  { A733_PHY_TGT_TOP  , A733_PHY_OP_ANDOR32   , 0x00100, 0x00000001, 0xFFFFFFFF },
  { A733_PHY_TGT_TOP  , A733_PHY_OP_POLL32    , 0x00900, 0x00000001, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_WRITE16   , 0x000A0, 0x00000270, 0x00000000 },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_ANDOR16   , 0x00098, 0x00000010, 0x0000FFFF },
  { A733_PHY_TGT_PHY  , A733_PHY_OP_ANDOR16   , 0x18000, 0x00000001, 0x0000FFFF },
  { A733_PHY_TGT_TOP  , A733_PHY_OP_ANDOR32   , 0x00004, 0x10000000, 0xFFFFFFFF },
};


STATIC
UINT64
PhyTargetBase (
  IN UINT8  Target
  )
{
  return (Target == A733_PHY_TGT_TOP) ? COMBO1_TOP_BASE : COMBO1_PHY_BASE;
}

/**
  Return the register base of a bank of the main pin controller.

  The A733 keeps a table of bank offsets at offset 0 of the PIO block, and the
  spacing is not uniform, so the offset is read from the hardware. The value is
  sanity checked because a bogus base here would scribble on unknown registers.
**/
STATIC
UINT64
PioBankBase (
  IN UINT32  Bank
  )
{
  UINT32  Offset;

  Offset = MmioRead32 ((UINTN)(A733_PIO_BASE + Bank * 4));

  if ((Offset > 0x1000) || ((Offset & 0x7F) != 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcie: PIO bank %u offset 0x%x looks wrong, using 0x%x\n",
      Bank, Offset, Bank * 0x80
      ));
    return A733_PIO_BASE + Bank * 0x80;
  }

  return A733_PIO_BASE + Offset;
}

/**
  Drive a pin as a push-pull output at the requested level.

  The level is written before the pin is switched to output so the pad does not
  briefly drive the wrong value.
**/
STATIC
VOID
PioDriveOutput (
  IN UINT64   BankBase,
  IN UINT32   Pin,
  IN BOOLEAN  High
  )
{
  UINTN   CfgReg;
  UINT32  Shift;

  if (High) {
    MmioOr32 ((UINTN)(BankBase + PIO_DATA), (UINT32)(1U << Pin));
  } else {
    MmioAnd32 ((UINTN)(BankBase + PIO_DATA), (UINT32)~(1U << Pin));
  }

  CfgReg = (UINTN)(BankBase + PIO_CFG0 + (Pin / 8) * 4);
  Shift  = (Pin % 8) * 4;
  MmioAndThenOr32 (CfgReg, ~(0xFU << Shift), PIO_MUX_OUTPUT << Shift);
}

/**
  Enable the clocks and release the resets the root complex and serdes need.

  Resets are released before the gates are opened, matching the vendor order.
  Nothing in here reads DBI, because DBI does not answer until this has run.
**/
STATIC
VOID
A733PcieClockInit (
  VOID
  )
{
  //
  // Assert every reset before releasing it.
  //
  // Every test of this driver so far has been a WARM reboot out of a running
  // Linux that already had PCIe up, so the core was already out of reset and
  // holding whatever state Linux left behind. Merely OR-ing the release bits
  // is then a no-op and the data link engine never restarts, which matches the
  // symptom: the PHY re-inits, the physical layer trains (SMLH), but RDLH
  // never comes up. The vendor U-Boot only ever runs from cold, so it never
  // had to do this.
  //
  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - asserting resets first\n"));
  MmioAnd32 (
    (UINTN)(A733_CCU_BASE + CCU_PCIE_BGR),
    (UINT32)~(PCIE_RST | PCIE_PWRUP_RST)
    );
  MmioAnd32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_BGR), (UINT32)~SERDES_RST);
  MmioAnd32 (
    (UINTN)(A733_CCU_BASE + CCU_ITS_BGR),
    (UINT32)~(ITS_PCIE0_RST | ITS_PCIE0_GATE)
    );
  MmioAnd32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AUX_CLK), (UINT32)~PCIE_AUX_GATE);
  MmioAnd32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AXI_SLV_CLK), (UINT32)~PCIE_AXI_SLV_GATE);
  MicroSecondDelay (10000);

  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - serdes reset and clock\n"));
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_BGR), SERDES_RST);
  //
  // Select the 600 MHz parent and divide by 6 for 100 MHz before ungating.
  //
  MmioAndThenOr32 (
    (UINTN)(A733_CCU_BASE + CCU_SERDES_PHY_CFG_CLK),
    ~(SERDES_PHY_CFG_M_MASK | SERDES_PHY_CFG_MUX_MASK),
    SERDES_PHY_CFG_MUX_PERI600 | SERDES_PHY_CFG_M_DIV6
    );
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_PHY_CFG_CLK), SERDES_PHY_CFG_GATE);
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: serdes phy cfg clk = 0x%08x (want mux 1, M 5, gated)\n",
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_PHY_CFG_CLK))
    ));

  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - PCIe resets\n"));
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_BGR), PCIE_RST | PCIE_PWRUP_RST);

  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - aux and AXI slave clocks\n"));
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AUX_CLK), PCIE_AUX_GATE);
  MmioOr32 (
    (UINTN)(A733_CCU_BASE + CCU_PCIE_AXI_SLV_CLK),
    PCIE_AXI_SLV_SRC_400M | PCIE_AXI_SLV_GATE
    );

  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - ITS\n"));
  MmioOr32 (
    (UINTN)(A733_CCU_BASE + CCU_ITS_BGR),
    ITS_PCIE0_RST | ITS_PCIE0_GATE
    );

  MicroSecondDelay (100);
}

/**
  Point combo-phy1 at PCIe and run the Cadence init sequence.

  combo-phy1 is shared with USB3, so the pipe select at SUBSYS_COMB1_PIPE has
  to be set before the PHY table runs.
**/
STATIC
EFI_STATUS
A733PciePhyInit (
  VOID
  )
{
  UINTN   Index;
  UINT64  Base;
  UINTN   Reg;
  UINT32  Elapsed;
  UINT32  Value;

  DEBUG ((DEBUG_ERROR, "SunxiPcie: serdes dcxo gate\n"));
  MmioOr32 ((UINTN)A733_DCXO_SERDES_REG, DCXO_SERDES1_GATING);

  DEBUG ((DEBUG_ERROR, "SunxiPcie: serdes subsystem\n"));
  MmioOr32 ((UINTN)(SERDES_SUBSYS_BASE + SUBSYS_PCIE_BGR), SUBSYS_PCIE_GATING);
  MmioOr32 ((UINTN)(SERDES_SUBSYS_BASE + SUBSYS_DBG_CTL), SUBSYS_DIS_COMBO1_AUTOGATE);

  DEBUG ((DEBUG_ERROR, "SunxiPcie: combo1 pipe -> PCIe\n"));
  MmioWrite32 ((UINTN)(SERDES_COMBO_BASE + SUBSYS_COMB1_PIPE), SUBSYS_COMB1_PIPE_PCIE);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: running PHY sequence, %u ops\n",
    (UINT32)(sizeof (mPciePhyInitOps) / sizeof (mPciePhyInitOps[0]))
    ));

  for (Index = 0; Index < sizeof (mPciePhyInitOps) / sizeof (mPciePhyInitOps[0]); Index++) {
    Base = PhyTargetBase (mPciePhyInitOps[Index].Target);
    Reg  = (UINTN)(Base + mPciePhyInitOps[Index].Offset);

    switch (mPciePhyInitOps[Index].Op) {
      case A733_PHY_OP_WRITE16:
        MmioWrite16 (Reg, (UINT16)mPciePhyInitOps[Index].Value);
        break;

      case A733_PHY_OP_WRITE32:
        MmioWrite32 (Reg, mPciePhyInitOps[Index].Value);
        break;

      case A733_PHY_OP_ANDOR16:
        MmioAndThenOr16 (
          Reg,
          (UINT16)mPciePhyInitOps[Index].Mask,
          (UINT16)mPciePhyInitOps[Index].Value
          );
        break;

      case A733_PHY_OP_ANDOR32:
        MmioAndThenOr32 (Reg, mPciePhyInitOps[Index].Mask, mPciePhyInitOps[Index].Value);
        break;

      case A733_PHY_OP_POLL32:
        Elapsed = 0;
        while (TRUE) {
          Value = MmioRead32 (Reg);
          if ((Value & mPciePhyInitOps[Index].Value) != 0) {
            DEBUG ((
              DEBUG_ERROR,
              "SunxiPcie: PMA ready after %u us (op %u, reg 0x%x = 0x%08x)\n",
              Elapsed, (UINT32)Index, (UINT32)mPciePhyInitOps[Index].Offset, Value
              ));
            break;
          }

          if (Elapsed >= PHY_POLL_TIMEOUT_US) {
            DEBUG ((
              DEBUG_ERROR,
              "SunxiPcie: PHY poll TIMEOUT at op %u, reg 0x%x = 0x%08x, wanted mask 0x%08x\n",
              (UINT32)Index,
              (UINT32)mPciePhyInitOps[Index].Offset,
              Value,
              mPciePhyInitOps[Index].Value
              ));
            return EFI_TIMEOUT;
          }

          MicroSecondDelay (PHY_POLL_INTERVAL_US);
          Elapsed += PHY_POLL_INTERVAL_US;
        }

        break;

      default:
        DEBUG ((DEBUG_ERROR, "SunxiPcie: bad PHY opcode %u at %u\n",
                mPciePhyInitOps[Index].Op, (UINT32)Index));
        return EFI_INVALID_PARAMETER;
    }
  }

  DEBUG ((DEBUG_ERROR, "SunxiPcie: PHY sequence complete\n"));
  return EFI_SUCCESS;
}

/**
  Stop link training. The vendor flow disables LTSSM before touching the reset
  GPIOs and configuring the core, and only re-enables it once everything is in
  place.
**/
STATIC
VOID
A733PcieLtssmDisable (
  VOID
  )
{
  MmioAnd32 (
    (UINTN)(A733_PCIE_APP_BASE + APP_LTSSM_CTRL),
    (UINT32)~APP_LINK_TRAINING
    );
}

/**
  Find a capability in the root port's own config space, walking the list.

  Safe to call only once the clocks are up, since this reads DBI.
**/
STATIC
UINT8
A733PcieFindCapability (
  IN UINT8  CapId
  )
{
  UINT8   Ptr;
  UINT16  Reg;
  UINTN   Guard;

  Ptr = (UINT8)(MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + DBI_CAP_LIST_PTR)) & CAP_ID_MASK);

  //
  // Bounded so a corrupt or unclocked config space cannot spin forever.
  //
  for (Guard = 0; (Ptr != 0) && (Guard < 48); Guard++) {
    Reg = MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + Ptr));
    if ((Reg & CAP_ID_MASK) > PCI_CAP_ID_MAX) {
      break;
    }

    if ((Reg & CAP_ID_MASK) == CapId) {
      return Ptr;
    }

    Ptr = (UINT8)((Reg & NEXT_CAP_PTR_MASK) >> 8);
  }

  return 0;
}

/**
  Tell the DesignWare core how wide and how fast the link is.

  This is the step whose absence left RDLH at 0 on the first hardware run: the
  physical layer trained happily, but without a configured lane count and
  target speed the data link layer never came up. Writes to LNKCAP need the
  read-only-write-enable bit, which is dropped again afterwards.
**/
STATIC
VOID
A733PcieSetLinkRate (
  VOID
  )
{
  UINT8   Cap;
  UINT32  Value;

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: DBI vendor/device = 0x%08x\n",
    MmioRead32 ((UINTN)A733_PCIE_DBI_BASE)
    ));

  MmioOr32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_MISC_CONTROL_1_CFG), DBI_RO_WR_EN);

  Cap = A733PcieFindCapability (PCI_CAP_ID_EXP);
  DEBUG ((DEBUG_ERROR, "SunxiPcie: PCIe express cap at 0x%02x\n", Cap));

  if (Cap != 0) {
    Value  = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKCTL2));
    Value &= ~PCI_EXP_LNKCTL2_TLS;
    Value |= PCIE_LINK_SPEED_GEN3;
    MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKCTL2), Value);

    Value  = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKCAP));
    Value &= ~PCI_EXP_LNKCAP_SLS;
    Value |= PCIE_LINK_SPEED_GEN3;
    MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKCAP), Value);
  }

  MmioAndThenOr32 (
    (UINTN)(A733_PCIE_DBI_BASE + DBI_PORT_LINK_CONTROL),
    ~PORT_LINK_MODE_MASK,
    PORT_LINK_MODE_1_LANE
    );

  MmioAndThenOr32 (
    (UINTN)(A733_PCIE_DBI_BASE + DBI_LINK_WIDTH_SPEED_CTRL),
    ~PORT_LOGIC_LINK_WIDTH_MASK,
    PORT_LOGIC_LINK_WIDTH_1_LANE
    );

  //
  // Root complex config: BARs, bus numbers and the command register. Without
  // these nothing can enumerate behind the bridge even once the link is up.
  //
  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_BAR0), 0x4);
  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_BAR1), 0x0);
  MmioAndThenOr32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_PRIMARY_BUS), 0xFF000000, RC_BUS_NUMBERS);
  MmioAndThenOr32 (
    (UINTN)(A733_PCIE_DBI_BASE + DBI_COMMAND),
    0xFFFF0000,
    CMD_IO | CMD_MEMORY | CMD_MASTER | CMD_SERR
    );

  MmioAnd32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_MISC_CONTROL_1_CFG), (UINT32)~DBI_RO_WR_EN);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: link configured 1 lane gen3, PORT_LINK=0x%08x WIDTH_SPEED=0x%08x\n",
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_PORT_LINK_CONTROL)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_LINK_WIDTH_SPEED_CTRL))
    ));
}

/**
  Enable link training and wait for both the physical and data link layers.
**/
STATIC
EFI_STATUS
A733PcieStartLink (
  VOID
  )
{
  UINT32  Elapsed;
  UINT32  Status;
  UINT32  Ltssm;

  //
  // Match the known-good PHY_CFG before training is enabled.
  //
  MmioOr32 ((UINTN)(A733_PCIE_APP_BASE + APP_PHY_CFG), APP_PHY_CFG_BIT21);
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: PHY_CFG now 0x%08x (target 0x00a023f0)\n",
    MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_PHY_CFG))
    ));

  Ltssm = MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_LTSSM_CTRL));
  DEBUG ((DEBUG_ERROR, "SunxiPcie: LTSSM_CTRL before = 0x%08x\n", Ltssm));

  MmioOr32 (
    (UINTN)(A733_PCIE_APP_BASE + APP_LTSSM_CTRL),
    APP_LINK_TRAINING | APP_DEVICE_TYPE_RC
    );

  for (Elapsed = 0; Elapsed < LINK_POLL_TIMEOUT_US; Elapsed += LINK_POLL_INTERVAL_US) {
    Status = MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_LINK_STAT));
    if ((Status & APP_LINK_UP) == APP_LINK_UP) {
      DEBUG ((
        DEBUG_ERROR,
        "SunxiPcie: LINK UP after %u us, LINK_STAT = 0x%08x\n",
        Elapsed, Status
        ));
      return EFI_SUCCESS;
    }

    if ((Elapsed % 100000) == 0) {
      DEBUG ((
        DEBUG_ERROR,
        "SunxiPcie:   t=%6u us LINK_STAT=0x%08x LTSSM=0x%08x PHY_CFG=0x%08x\n",
        Elapsed,
        Status,
        MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_LTSSM_CTRL)),
        MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_PHY_CFG))
        ));
    }

    MicroSecondDelay (LINK_POLL_INTERVAL_US);
  }

  Status = MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_LINK_STAT));
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: link DID NOT train, LINK_STAT = 0x%08x (SMLH=%u RDLH=%u)\n",
    Status,
    (Status & APP_SMLH_LINK_UP) ? 1 : 0,
    (Status & APP_RDLH_LINK_UP) ? 1 : 0
    ));
  return EFI_TIMEOUT;
}

/**
  Entry point.

  Returns EFI_SUCCESS even when the link fails to train, so that a PCIe
  problem never stops the rest of the firmware from booting. The UART log is
  the record of what happened.
**/
EFI_STATUS
EFIAPI
SunxiPcieDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT64      PortD;
  UINT64      PortL;

  DEBUG ((DEBUG_ERROR, "SunxiPcie: start, bringing up the root complex\n"));

  PortD = PioBankBase (PIO_BANK_D);
  PortL = A733_R_PIO_BASE;
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: PD bank @0x%lx (expect 0x02000200), PL bank @0x%lx\n",
    PortD, PortL
    ));

  //
  // Slot power on, and hold the endpoint in reset while the clocks and PHY
  // come up.
  //
  //
  // Power-cycle the slot rather than just asserting power. The vendor flow
  // drives it low for 100 ms first, and an endpoint that was left in a strange
  // state by a previous boot needs that to come back cleanly.
  //
  DEBUG ((DEBUG_ERROR, "SunxiPcie: PL3 power cycle, PD22 PERST# low\n"));
  PioDriveOutput (PortL, PIN_POWER, FALSE);
  MicroSecondDelay (100000);
  PioDriveOutput (PortL, PIN_POWER, TRUE);

  PioDriveOutput (PortD, PIN_PERST, FALSE);
  MicroSecondDelay (1000);

  A733PcieClockInit ();

  Status = A733PciePhyInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiPcie: PHY init failed - %r, giving up\n", Status));
    return EFI_SUCCESS;
  }

  //
  // PCI Express wants PERST# held for a while after power and clocks are
  // stable. 10 ms is generous for a soldered-down single lane slot and costs
  // nothing at boot.
  //
  //
  // Reset dance, following the vendor order exactly: training off, WAKE high,
  // then PERST# low for a full 100 ms before release. PCI Express requires
  // 100 ms here; the 10 ms used previously got the physical layer up (SMLH)
  // but never completed the data link layer (RDLH stayed 0).
  //
  DEBUG ((DEBUG_ERROR, "SunxiPcie: LTSSM off, WAKE high, PERST# low 100ms\n"));
  A733PcieLtssmDisable ();

  PioDriveOutput (PortD, PIN_WAKE, TRUE);
  PioDriveOutput (PortD, PIN_PERST, FALSE);
  MicroSecondDelay (100000);
  PioDriveOutput (PortD, PIN_PERST, TRUE);
  MicroSecondDelay (100000);
  DEBUG ((DEBUG_ERROR, "SunxiPcie: PERST# released\n"));
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: BEFORE training: LINK_STAT=0x%08x PHY_CFG=0x%08x (linux golden PHY_CFG=0x00a023f0)\n",
    MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_LINK_STAT)),
    MmioRead32 ((UINTN)(A733_PCIE_APP_BASE + APP_PHY_CFG))
    ));

  //
  // DBI is answerable from here on, because the clocks and the PHY are up.
  //
  A733PcieSetLinkRate ();

  Status = A733PcieStartLink ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcie: no link. If the PHY sequence ran clean, suspect the 1.8V PHY\n"
      "           rail (bldo1 on the axp8191 at i2c 0x36), which nothing here\n"
      "           enables yet.\n"
      ));
    return EFI_SUCCESS;
  }

  //
  // DBI is only safe to touch now. Read the root port's own vendor/device as
  // proof the block answers, which is exactly what hung the previous driver
  // when it was attempted before the clocks were up.
  //
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: DBI vendor/device = 0x%08x\n",
    MmioRead32 ((UINTN)A733_PCIE_DBI_BASE)
    ));

  DEBUG ((DEBUG_ERROR, "SunxiPcie: root complex is up\n"));
  return EFI_SUCCESS;
}
