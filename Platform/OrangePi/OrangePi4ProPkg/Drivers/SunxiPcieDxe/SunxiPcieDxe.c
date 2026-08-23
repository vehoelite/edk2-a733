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
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
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
//
// SUNXI_CCU_M_WITH_MUX_GATE: M in bits 0-4, mux in bits 24-26, gate bit 31.
// Parents are { pll-peri0-600m, pll-peri0-600m, pll-peri0-400m }, so mux 2 is
// the 400 MHz source the vendor selects, with the divider at 1.
//
// These are multi-bit fields, so they have to be assigned, not OR-ed. OR-ing
// 2 into a mux that already reads 1 would give 3, which is off the end of a
// three-entry parent list.
//
#define   PCIE_AXI_SLV_M_MASK         0x0000001FU
#define   PCIE_AXI_SLV_MUX_MASK       (0x7U << 24)
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
//
// From the A733 user manual, 18.3.6.1 PCIe System Initialization, step 4:
// "Write 1 to the MBUS_MAT_CLK_GATING_REG (bit[28]) to enable MBUS_MCLK clock".
// This step was missing entirely. Both of these gating registers are key
// protected -- the key has to accompany the write or it is silently dropped,
// which is why a plain bit set would have looked like it worked and done
// nothing. Register numbers and keys come from the BSP CCU driver
// (serdes_mbus_gate_clk / serdes_ahb_gate_clk).
//
#define CCU_MBUS_MAT_CLK_GATING       0x05E0
#define   MBUS_MASTER_KEY_VALUE       0x41055800
#define   MBUS_SERDES_GATE            BIT28
#define CCU_AHB_MAT_CLK_GATING        0x05C0
#define   AHB_MASTER_KEY_VALUE        0x010000FF
#define   AHB_SERDES_GATE             BIT8
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
// Internal address translation unit. This core uses the "unroll" iATU layout,
// where each region has its own register block at DBI + 0x300000 + n * 0x200
// rather than the older single viewport window. The DBI resource on this SoC
// is 0x06000000-0x0647ffff, so 0x06300000 falls inside it.
//
// Nothing reaches the endpoint without this. The link can be fully up, with
// the data link layer active, and every config or memory cycle aimed at the
// device still goes nowhere, because no outbound region maps CPU addresses
// onto PCIe transactions. That is why the root complex came up but the drive
// never enumerated.
//
// Apertures come from the working Linux boot, which reports:
//     22000000-27ffffff : pcie@6000000
//       22100000-221fffff : PCI Bus 0000:01
//         22100000-22103fff : 0000:01:00.0 -> nvme
//       22200000-2220ffff : 0000:00:00.0   -> config window
//
#define A733_PCIE_ATU_BASE            (A733_PCIE_DBI_BASE + 0x300000ULL)
#define   ATU_REGION_STRIDE           0x200
#define   ATU_REGION_CTRL1            0x00
#define     ATU_TYPE_MEM              0x0
#define     ATU_TYPE_CFG0             0x4
#define   ATU_REGION_CTRL2            0x04
#define     ATU_REGION_ENABLE         BIT31
#define   ATU_LOWER_BASE              0x08
#define   ATU_UPPER_BASE              0x0C
#define   ATU_LIMIT                   0x10
#define   ATU_LOWER_TARGET            0x14
#define   ATU_UPPER_TARGET            0x18

//
// Region 0 maps the config window onto bus 1. The PCIe target address for a
// config cycle is bus << 24 | device << 19 | function << 16, so bus 1 device 0
// function 0 is 0x01000000.
//
#define A733_PCIE_CFG_BASE            0x22200000ULL
#define A733_PCIE_CFG_SIZE            0x00010000ULL
#define A733_PCIE_CFG_TARGET_BUS1     0x01000000U

//
// Region 1 maps memory behind the bridge one to one, which is what the BSP
// does and what keeps BAR values meaningful on both sides.
//
#define A733_PCIE_MEM_BASE            0x22100000ULL
#define A733_PCIE_MEM_SIZE            0x00100000ULL

//
// DesignWare link configuration, in DBI. The core has to be told the lane
// count and the target speed BEFORE link training is enabled; leaving it at
// reset defaults gets the physical layer up (SMLH) but the data link layer
// never completes (RDLH stays 0), which is exactly what the first hardware
// run showed.
//
#define   DBI_PORT_LINK_CONTROL           0x710
#define   DBI_FILTER_MASK_1               0x71C
#define   DBI_FILTER_MASK_2               0x720
//
// VC0 receive queue control. These hold the credit values the core ADVERTISES
// in its InitFC1 DLLPs, and they are sampled during flow control
// initialisation only -- writing them after the link is up does nothing
// without a retrain. We never programmed them at all, so whatever the reset
// state is, that is what the endpoint was being offered. If those values are
// illegal the endpoint stalls in its own DL_Init and never sends InitFC2
// back, which is exactly the signature we have: L0 stable, zero AER errors in
// both directions, TX credits stuck at zero, DLLLA never set.
//
// The values below are read back from the working vendor BSP boot on this
// same board with this same drive, where LNKSTA reads 0xf013 (Gen3 x1, DLLLA
// set) and the TX credit registers are non-zero.
//
#define   DBI_VC0_P_RX_Q_CTRL             0x748
#define     VC0_P_RX_Q_GOLDEN             0x4523E060
#define   DBI_VC0_NP_RX_Q_CTRL            0x74C
#define     VC0_NP_RX_Q_GOLDEN            0x0523E00F
#define   DBI_VC0_CPL_RX_Q_CTRL           0x750
#define     VC0_CPL_RX_Q_GOLDEN           0x05800000
#define     PORT_LINK_MODE_MASK           (0x3FU << 16)
#define     PORT_LINK_MODE_1_LANE         (0x01U << 16)
#define   DBI_LINK_WIDTH_SPEED_CTRL       0x80C
#define     PORT_LOGIC_LINK_WIDTH_MASK    (0x1FFU << 8)
#define     PORT_LOGIC_LINK_WIDTH_1_LANE  (0x001U << 8)
//
// DesignWare port-logic debug. The vendor glue LINK_STAT bit is not trustworthy
// on its own: it can be sticky, and sampling it every 100ms would alias a link
// that trains, drops and retrains every few milliseconds into a steady "up".
// PL_DEBUG0 carries the real LTSSM state, so sample it in a tight loop and
// build a histogram.
//
#define   DBI_PL_DEBUG0                   0x728
#define     PL_DEBUG0_LTSSM_MASK          0x3F
#define   DBI_PL_DEBUG1                   0x72C
#define     PL_DEBUG1_LINK_UP             BIT4
#define     PL_DEBUG1_LINK_IN_TRAINING    BIT29
//
// Transmit flow-control credit status. If these are non-zero the RC has
// received the endpoint InitFC1 DLLPs, i.e. the far side data link is alive and
// the failure is in completing FC_INIT2. If they stay zero the RC has never
// seen a single valid InitFC DLLP.
//
#define   DBI_TX_P_FC_CREDIT_STATUS       0x730
#define   DBI_TX_NP_FC_CREDIT_STATUS      0x734
#define   DBI_TX_CPL_FC_CREDIT_STATUS     0x738
#define   DBI_QUEUE_STATUS                0x73C
//
// PCIe extended capabilities start at 0x100. Each header holds the capability
// id in bits 15:0 and the offset of the next one in bits 31:20.
//
// DLLPs carry a CRC and have no retry: a corrupt one is silently discarded. So
// a receive path that mangles symbols can hold L0 happily -- the LTSSM only
// needs ordered sets and framing -- while dropping every InitFC DLLP, which is
// exactly the stable-L0-with-zero-credits picture we see. Counting correctable
// receiver errors distinguishes "InitFC1s arrive but fail CRC" from "nothing
// arrives at all".
//
#define   DBI_EXT_CAP_BASE                0x100
#define     EXT_CAP_ID_MASK               0x0000FFFFU
#define     EXT_CAP_NEXT_SHIFT            20
#define     EXT_CAP_ID_AER                0x0001
#define     EXT_CAP_ID_VNDR               0x000B      // RAS D.E.S. lives here
#define   AER_UNCORR_STATUS               0x04
#define   AER_CORR_STATUS                 0x10
#define     AER_CORR_RECEIVER_ERROR       BIT0
#define     AER_CORR_BAD_TLP              BIT6
#define     AER_CORR_BAD_DLLP             BIT7
#define     AER_CORR_REPLAY_ROLLOVER      BIT8
#define     AER_CORR_REPLAY_TIMER         BIT12
#define   AER_CORR_MASK                   0x14
//
// Synopsys debug event counters, in the vendor extended capability. Layout and
// event numbering taken from the mainline kernel driver
// drivers/pci/controller/dwc/pcie-designware-debugfs.c, which is public, rather
// than from the Synopsys databook.
//
#define   RAS_EVENT_COUNTER_CTRL          0x08
#define     RAS_GROUP_SHIFT               24          // bits 27:24
#define     RAS_EVENT_SHIFT               16          // bits 23:16
#define     RAS_LANE_SHIFT                8           // bits 11:8
#define     RAS_COUNTER_STATUS            BIT7
#define     RAS_ENABLE_SHIFT              2           // bits 4:2
#define     RAS_PER_EVENT_ON              0x3
#define     RAS_PER_EVENT_OFF             0x1
//
// Standard Synopsys encoding for the enable field: 000 means no change, which
// is why the kernel can clear the field while re-selecting an event without
// disabling anything. 111 turns every counter on at once, which is what we
// want here -- enabling them one at a time around each read never let them
// accumulate.
//
#define     RAS_ALL_EVENT_ON              0x7
#define     RAS_ALL_EVENT_OFF             0x5
#define   RAS_EVENT_COUNTER_DATA          0x0C
#define     PCI_EXP_LNKSTA                0x12    // cap + 0x12
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
#define     PCIE_LINK_SPEED_GEN1          0x1
#define     PCIE_LINK_SPEED_GEN3          0x3
//
// Target link speed. Set to GEN1 deliberately as an experiment.
//
// The link currently reports LNKSTA speed Gen1 even though we advertise Gen3,
// and the LTSSM histogram is full of Recovery.Lock / Recovery.RcvrCfg /
// Recovery.Idle excursions -- which is exactly the path a Gen3 speed change
// takes. The theory is that the core keeps retrying a speed change that never
// succeeds, and each attempt disturbs the link before flow control
// initialisation can finish, so credits stay at zero and DLLLA never sets.
//
// There is a known Gen3 speed-change/equalisation bug in the A733 BSP driver
// that leaves DLActive clear on some drives (Phison E27T especially). That
// exact bug is not ours -- Linux brings this WD drive up at Gen3 on this same
// board, LNKSTA 0xf013 with DLLLA set -- but the same class of failure could
// bite our less complete bring-up. Advertising Gen1 skips the speed-change
// path entirely, so if RDLH asserts at Gen1 the whole class is confirmed and
// the work becomes the equalisation sequence rather than flow control.
//
#define     PCIE_LINK_SPEED_TARGET        PCIE_LINK_SPEED_GEN1

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
  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - serdes AHB and MBUS gates\n"));
  MmioOr32 (
    (UINTN)(A733_CCU_BASE + CCU_AHB_MAT_CLK_GATING),
    AHB_MASTER_KEY_VALUE | AHB_SERDES_GATE
    );
  MmioOr32 (
    (UINTN)(A733_CCU_BASE + CCU_MBUS_MAT_CLK_GATING),
    MBUS_MASTER_KEY_VALUE | MBUS_SERDES_GATE
    );
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: AHB gate 0x%08x, MBUS gate 0x%08x\n",
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_AHB_MAT_CLK_GATING)),
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_MBUS_MAT_CLK_GATING))
    ));

  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - serdes phy cfg clock\n"));
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
  //
  // The manual (18.3.6.1 step 2) says 2'b10 into bit[17:16] here, i.e. bit16
  // clear. Tried on hardware: with bit16 clear the PHY PMA poll times out, so
  // PCIE0_PWRUP_RST really does have to be de-asserted too. The vendor U-Boot
  // sets both bits and that is what works. As with the serdes register above,
  // the manual is reliable for the ORDER of these steps but not for the bit
  // numbering of the BGR registers on this part.
  //
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_BGR), PCIE_RST | PCIE_PWRUP_RST);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: SERDES_BGR=0x%08x PCIE_BGR=0x%08x (want bit17 set, bit16 clear)\n",
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_BGR)),
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_BGR))
    ));

  DEBUG ((DEBUG_ERROR, "SunxiPcie: CCU - aux and AXI slave clocks\n"));
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AUX_CLK), PCIE_AUX_GATE);
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: AUX_CLK before=0x%08x AXI_SLV before=0x%08x\n",
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AUX_CLK)),
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AXI_SLV_CLK))
    ));

  //
  // Assign the mux and divider rather than OR-ing them. These are multi-bit
  // fields: OR-ing 2 into a mux that already reads 1 gives 3, which is off the
  // end of a three-entry parent list, and the divider was never cleared at all.
  // Verified on hardware that this boots.
  //
  //
  // Assigning the mux/divider explicitly (clear then set mux 2) HANGS the
  // board: EDK2 stops with a silent console, which is what a dead AXI slave
  // clock looks like when the first DBI access goes out. The likely reason is
  // that pll-peri0-400m, the mux 2 parent, is not running this early, so
  // forcing that parent kills the clock. The vendor OR is safe because in
  // practice it does not change the field.
  //
  // Left as the vendor form deliberately. The before/after prints above exist
  // to capture what the field actually holds -- decide from that reading, not
  // from the parent table.
  //
  MmioOr32 (
    (UINTN)(A733_CCU_BASE + CCU_PCIE_AXI_SLV_CLK),
    PCIE_AXI_SLV_SRC_400M | PCIE_AXI_SLV_GATE
    );

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: AUX_CLK after=0x%08x AXI_SLV after=0x%08x (want mux 2, M 0, gated)\n",
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AUX_CLK)),
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_PCIE_AXI_SLV_CLK))
    ));

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

  //
  // De-assert the serdes reset HERE, after the serdes clock and the dcxo
  // gate are already running -- this is what the Linux BSP does and it is
  // the one ordering difference between it and the vendor U-Boot:
  //
  //     clk_set_rate(serdes_clk, 100000000); clk_prepare_enable(serdes_clk);
  //     clk_prepare_enable(dcxo_serdes1_clk);
  //     reset_control_deassert(sunxi_cphy->serdes_reset);   <-- Linux only
  //
  // We were releasing it in clock init, before the serdes clock existed.
  // Releasing a block from reset with no clock running lets it latch a bad
  // state, which fits a PHY that reports PMA ready while the data link
  // never comes up.
  //
  DEBUG ((DEBUG_ERROR, "SunxiPcie: serdes reset de-assert (post-clock)\n"));
  MmioOr32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_BGR), SERDES_RST);
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: SERDES_BGR=0x%08x\n",
    MmioRead32 ((UINTN)(A733_CCU_BASE + CCU_SERDES_BGR))
    ));
  MicroSecondDelay (1000);

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
    Value |= PCIE_LINK_SPEED_TARGET;
    MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKCTL2), Value);

    Value  = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKCAP));
    Value &= ~PCI_EXP_LNKCAP_SLS;
    Value |= PCIE_LINK_SPEED_TARGET;
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
  // Report what the advertised flow control credits currently are, then set
  // them to the values the working BSP uses. Printed before and after so a
  // boot log says whether the reset state was already correct -- if these
  // read back the same as the golden values, this hypothesis is dead and the
  // fault is elsewhere.
  //
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: VC0 credits BEFORE  P=0x%08x NP=0x%08x CPL=0x%08x\n",
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_P_RX_Q_CTRL)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_NP_RX_Q_CTRL)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_CPL_RX_Q_CTRL))
    ));
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: filter masks        F1=0x%08x F2=0x%08x (BSP: 0x00000140 0x00000000)\n",
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_FILTER_MASK_1)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_FILTER_MASK_2))
    ));

  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_P_RX_Q_CTRL),   VC0_P_RX_Q_GOLDEN);
  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_NP_RX_Q_CTRL),  VC0_NP_RX_Q_GOLDEN);
  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_CPL_RX_Q_CTRL), VC0_CPL_RX_Q_GOLDEN);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: VC0 credits AFTER   P=0x%08x NP=0x%08x CPL=0x%08x\n",
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_P_RX_Q_CTRL)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_NP_RX_Q_CTRL)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_VC0_CPL_RX_Q_CTRL))
    ));

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
    "SunxiPcie: link configured 1 lane, target speed gen%u, PORT_LINK=0x%08x WIDTH_SPEED=0x%08x\n",
    (UINT32)PCIE_LINK_SPEED_TARGET,
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_PORT_LINK_CONTROL)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_LINK_WIDTH_SPEED_CTRL))
    ));
}

/**
  Program one outbound iATU region.
**/
STATIC
VOID
A733PcieAtuOutbound (
  IN UINT32  Index,
  IN UINT32  Type,
  IN UINT64  CpuBase,
  IN UINT64  Size,
  IN UINT64  PciTarget
  )
{
  UINTN   Base;
  UINT32  Limit;

  Base  = (UINTN)(A733_PCIE_ATU_BASE + (Index * ATU_REGION_STRIDE));
  Limit = (UINT32)(CpuBase + Size - 1);

  MmioWrite32 (Base + ATU_LOWER_BASE,   (UINT32)CpuBase);
  MmioWrite32 (Base + ATU_UPPER_BASE,   (UINT32)RShiftU64 (CpuBase, 32));
  MmioWrite32 (Base + ATU_LIMIT,        Limit);
  MmioWrite32 (Base + ATU_LOWER_TARGET, (UINT32)PciTarget);
  MmioWrite32 (Base + ATU_UPPER_TARGET, (UINT32)RShiftU64 (PciTarget, 32));
  MmioWrite32 (Base + ATU_REGION_CTRL1, Type);
  MmioWrite32 (Base + ATU_REGION_CTRL2, ATU_REGION_ENABLE);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: iATU%u type %u cpu 0x%lx size 0x%lx -> pci 0x%lx ctrl2=0x%08x\n",
    Index,
    Type,
    CpuBase,
    Size,
    PciTarget,
    MmioRead32 (Base + ATU_REGION_CTRL2)
    ));
}

/**
  Set up address translation and prove it works by reading the endpoint's
  config space.

  A successful read here is the whole point: it means a CPU access to the
  config window was translated into a real config cycle, crossed the link and
  came back with the device answering. If this prints the drive's vendor and
  device id, then everything left is EDK2 plumbing -- a host bridge and
  PciBusDxe -- rather than anything to do with the hardware.
**/
STATIC
VOID
A733PcieSetupAtu (
  VOID
  )
{
  UINT32  Id;
  UINT32  ClassRev;

  A733PcieAtuOutbound (
    0,
    ATU_TYPE_CFG0,
    A733_PCIE_CFG_BASE,
    A733_PCIE_CFG_SIZE,
    A733_PCIE_CFG_TARGET_BUS1
    );

  A733PcieAtuOutbound (
    1,
    ATU_TYPE_MEM,
    A733_PCIE_MEM_BASE,
    A733_PCIE_MEM_SIZE,
    A733_PCIE_MEM_BASE
    );

  //
  // The root port's secondary and subordinate bus numbers have to say bus 1
  // lives behind it, or a config cycle aimed at bus 1 is dropped by the bridge.
  //
  MmioOr32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_MISC_CONTROL_1_CFG), DBI_RO_WR_EN);
  MmioAndThenOr32 (
    (UINTN)(A733_PCIE_DBI_BASE + DBI_PRIMARY_BUS),
    0xFF000000,
    RC_BUS_NUMBERS
    );
  MmioAnd32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_MISC_CONTROL_1_CFG), (UINT32)~DBI_RO_WR_EN);

  Id       = MmioRead32 ((UINTN)A733_PCIE_CFG_BASE);
  ClassRev = MmioRead32 ((UINTN)(A733_PCIE_CFG_BASE + 0x08));

  if (Id == 0xFFFFFFFF || Id == 0x00000000) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcie: endpoint config read gave 0x%08x -- nothing answering yet\n",
      Id
      ));
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: ENDPOINT FOUND vendor 0x%04x device 0x%04x class 0x%06x\n",
    Id & 0xFFFF,
    (Id >> 16) & 0xFFFF,
    ClassRev >> 8
    ));
}

/**
  Walk the extended capability list looking for one id.
**/
STATIC
UINT32
A733PcieFindExtCapability (
  IN UINT16  CapId
  )
{
  UINT32  Offset;
  UINT32  Header;
  UINTN   Guard;

  Offset = DBI_EXT_CAP_BASE;

  for (Guard = 0; (Offset != 0) && (Offset < 0x1000) && (Guard < 64); Guard++) {
    Header = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Offset));
    if ((Header == 0) || (Header == 0xFFFFFFFF)) {
      break;
    }

    if ((Header & EXT_CAP_ID_MASK) == CapId) {
      return Offset;
    }

    Offset = Header >> EXT_CAP_NEXT_SHIFT;
  }

  return 0;
}

/**
  Read one Synopsys debug event counter.
**/
STATIC
UINT32
A733PcieReadRasCounter (
  IN UINT32  RasCap,
  IN UINT8   Group,
  IN UINT8   Event
  )
{
  UINTN   Ctrl;
  UINT32  Value;

  Ctrl = (UINTN)(A733_PCIE_DBI_BASE + RasCap + RAS_EVENT_COUNTER_CTRL);

  //
  // Select only. The enable field is left at 000 (no change) so selecting an
  // event never disturbs the counters, which are turned on once, globally.
  //
  Value  = MmioRead32 (Ctrl);
  Value &= ~((0xFU << RAS_GROUP_SHIFT) | (0xFFU << RAS_EVENT_SHIFT) |
             (0xFU << RAS_LANE_SHIFT) | (0x7U << RAS_ENABLE_SHIFT));
  Value |= ((UINT32)Group << RAS_GROUP_SHIFT) | ((UINT32)Event << RAS_EVENT_SHIFT);
  MmioWrite32 (Ctrl, Value);

  return MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + RasCap + RAS_EVENT_COUNTER_DATA));
}

/**
  Sweep the debug event counters that matter for a link that sits in L0 without
  ever completing flow control initialisation.

  AER says there are no correctable errors at all, so this is the finer
  instrument: elastic buffer over/underrun would mean a clocking mismatch,
  decode or disparity or framing errors would mean symbol corruption, and
  fc_timeout would say flow control initialisation is being attempted and
  timing out rather than never starting.
**/
STATIC
VOID
A733PcieReportRasCounters (
  VOID
  )
{
  STATIC CONST struct {
    CONST CHAR8    *Name;
    UINT8          Group;
    UINT8          Event;
  } Counters[] = {
    { "ebuf_overflow",        0x0, 0x0 },
    { "ebuf_underrun",        0x0, 0x1 },
    { "decode_err",           0x0, 0x2 },
    { "disparity_err",        0x0, 0x3 },
    { "sync_header_err",      0x0, 0x5 },
    { "rx_valid_deassert",    0x0, 0x6 },
    { "detect_ei_infer",      0x1, 0x5 },
    { "receiver_err",         0x1, 0x6 },
    { "rx_recovery_req",      0x1, 0x7 },
    { "n_fts_timeout",        0x1, 0x8 },
    { "framing_err",          0x1, 0x9 },
    { "framing_err_in_l0",    0x1, 0xC },
    { "bad_tlp",              0x2, 0x0 },
    { "lcrc_err",             0x2, 0x1 },
    { "bad_dllp",             0x2, 0x2 },
    { "rx_nak_dllp",          0x2, 0x5 },
    { "tx_nak_dllp",          0x2, 0x6 },
    { "fc_timeout",           0x3, 0x0 },
    { "l0_to_recovery",       0x5, 0x0 }
  };

  UINT32  RasCap;
  UINTN   Index;
  UINT32  Value;

  RasCap = A733PcieFindExtCapability (EXT_CAP_ID_VNDR);
  if (RasCap == 0) {
    DEBUG ((DEBUG_ERROR, "SunxiPcie: no vendor debug capability found\n"));
    return;
  }

  //
  // Turn every counter on in one write, then let the link run.
  //
  Value  = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + RasCap + RAS_EVENT_COUNTER_CTRL));
  Value &= ~(0x7U << RAS_ENABLE_SHIFT);
  Value |= (RAS_ALL_EVENT_ON << RAS_ENABLE_SHIFT);
  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + RasCap + RAS_EVENT_COUNTER_CTRL), Value);

  Value = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + RasCap + RAS_EVENT_COUNTER_CTRL));
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: counter ctrl after all-on = 0x%08x (status bit %u)\n",
    Value,
    (Value & RAS_COUNTER_STATUS) ? 1 : 0
    ));

  MicroSecondDelay (300000);

  DEBUG ((DEBUG_ERROR, "SunxiPcie: debug event counters at cap 0x%03x\n", RasCap));
  for (Index = 0; Index < sizeof (Counters) / sizeof (Counters[0]); Index++) {
    Value = A733PcieReadRasCounter (RasCap, Counters[Index].Group, Counters[Index].Event);
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcie:   %-20a = %u\n",
      Counters[Index].Name,
      Value
      ));
  }
}

/**
  Clear the correctable error status, let the link run, and report what came
  back. Accumulating receiver or bad-DLLP errors while the LTSSM sits in L0
  means the receive path is corrupting symbols, which silently destroys the
  InitFC DLLPs that flow control initialisation depends on.
**/
STATIC
VOID
A733PcieReportAer (
  VOID
  )
{
  UINT32  Aer;
  UINT32  First;
  UINT32  Second;

  Aer = A733PcieFindExtCapability (EXT_CAP_ID_AER);
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: AER cap at 0x%03x, RAS/vendor cap at 0x%03x\n",
    Aer,
    A733PcieFindExtCapability (EXT_CAP_ID_VNDR)
    ));

  if (Aer == 0) {
    return;
  }

  First = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Aer + AER_CORR_STATUS));

  //
  // Write-1-to-clear, then let it accumulate for a while.
  //
  MmioWrite32 ((UINTN)(A733_PCIE_DBI_BASE + Aer + AER_CORR_STATUS), 0xFFFFFFFF);
  MicroSecondDelay (200000);
  Second = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Aer + AER_CORR_STATUS));

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: AER corr before=0x%08x after-200ms=0x%08x uncorr=0x%08x mask=0x%08x\n",
    First,
    Second,
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Aer + AER_UNCORR_STATUS)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + Aer + AER_CORR_MASK))
    ));

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie:   rx_err=%u bad_tlp=%u bad_dllp=%u replay_ro=%u replay_to=%u\n",
    (Second & AER_CORR_RECEIVER_ERROR) ? 1 : 0,
    (Second & AER_CORR_BAD_TLP) ? 1 : 0,
    (Second & AER_CORR_BAD_DLLP) ? 1 : 0,
    (Second & AER_CORR_REPLAY_ROLLOVER) ? 1 : 0,
    (Second & AER_CORR_REPLAY_TIMER) ? 1 : 0
    ));
}

/**
  Sample the real LTSSM state from PL_DEBUG0 as fast as possible and report a
  histogram, so a link that is bouncing cannot hide behind a slow poll of the
  vendor status bit.
**/
STATIC
VOID
A733PcieLtssmHistogram (
  VOID
  )
{
  UINT32  Counts[64];
  UINT32  Index;
  UINT32  Sample;
  UINT32  State;
  UINT32  Dbg1Training;
  UINT32  Dbg1Up;

  ZeroMem (Counts, sizeof (Counts));
  Dbg1Training = 0;
  Dbg1Up       = 0;

  //
  // No delay in the loop at all: MMIO reads are slow enough on their own, and
  // any added delay would reintroduce the aliasing this is meant to expose.
  //
  for (Index = 0; Index < 200000; Index++) {
    Sample = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_PL_DEBUG0));
    State  = Sample & PL_DEBUG0_LTSSM_MASK;
    Counts[State]++;

    Sample = MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_PL_DEBUG1));
    if ((Sample & PL_DEBUG1_LINK_IN_TRAINING) != 0) {
      Dbg1Training++;
    }

    if ((Sample & PL_DEBUG1_LINK_UP) != 0) {
      Dbg1Up++;
    }
  }

  DEBUG ((DEBUG_ERROR, "SunxiPcie: LTSSM histogram over 200000 samples\n"));
  for (Index = 0; Index < 64; Index++) {
    if (Counts[Index] != 0) {
      DEBUG ((
        DEBUG_ERROR,
        "SunxiPcie:   state 0x%02x : %u\n",
        Index,
        Counts[Index]
        ));
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie:   PL_DEBUG1 link_up=%u in_training=%u (of 200000)\n",
    Dbg1Up,
    Dbg1Training
    ));
}

/**
  Report the flow control credit state and the link status register.
**/
STATIC
VOID
A733PcieReportDl (
  IN UINT8  Cap
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcie: TX FC credits P=0x%08x NP=0x%08x CPL=0x%08x QUEUE=0x%08x\n",
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_TX_P_FC_CREDIT_STATUS)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_TX_NP_FC_CREDIT_STATUS)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_TX_CPL_FC_CREDIT_STATUS)),
    MmioRead32 ((UINTN)(A733_PCIE_DBI_BASE + DBI_QUEUE_STATUS))
    ));

  if (Cap != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcie: LNKSTA=0x%04x (speed %u, width %u, training %u, DLLLA %u)\n",
      MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKSTA)),
      MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKSTA)) & 0xF,
      (MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKSTA)) >> 4) & 0x3F,
      (MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKSTA)) >> 11) & 1,
      (MmioRead16 ((UINTN)(A733_PCIE_DBI_BASE + Cap + PCI_EXP_LNKSTA)) >> 13) & 1
      ));
  }
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

  //
  // The glue status bit says the physical layer is up. Get ground truth from
  // the DesignWare port logic before believing it.
  //
  A733PcieLtssmHistogram ();
  A733PcieReportDl (A733PcieFindCapability (PCI_CAP_ID_EXP));
  A733PcieReportAer ();
  A733PcieReportRasCounters ();

  //
  // Read the credits a second time after the AER dwell. If they are still zero
  // after another 200 ms, flow control really never completes rather than
  // simply being slow.
  //
  A733PcieReportDl (A733PcieFindCapability (PCI_CAP_ID_EXP));

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
  // Assert slot power and leave it alone. Do NOT power-cycle it.
  //
  // This used to drop PL3 for 100 ms first, copied from the vendor U-Boot. The
  // Linux BSP, which brings this drive up reliably on this board, never touches
  // the power GPIO at all -- it only drives PERST#. Cutting the slot rail and
  // restoring it 100 ms later means PERST# is released into a drive whose
  // controller is still starting up: an NVMe device commonly needs far longer
  // than that before its data link layer will talk. That matches exactly what
  // the diagnostics show -- the LTSSM parks in L0, the receive path reports
  // zero errors of any kind, and yet no InitFC DLLPs ever arrive, so flow
  // control credits stay at zero and rdlh_link_up never asserts.
  //
  DEBUG ((DEBUG_ERROR, "SunxiPcie: PL3 power high (no cycle), PD22 PERST# low\n"));
  PioDriveOutput (PortL, PIN_POWER, TRUE);
  MicroSecondDelay (10000);

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

  A733PcieSetupAtu ();

  DEBUG ((DEBUG_ERROR, "SunxiPcie: root complex is up\n"));
  return EFI_SUCCESS;
}
