/** @file
  SunxiPcieDxe - PCIe / NVMe bring-up for Allwinner A733 (sun60iw2).

  Architecture (per BSP `bsp/drivers/pcie/pcie-sunxi-{rc,plat}.c`,
                compatible "allwinner,sunxi-pcie-v300-rc")
  -----------------------------------------------------------------------
  The PCIe controller is a Synopsys DesignWare RC. Two register windows
  are used by the BSP driver, both anchored at the DBI base (0x06000000):

      DBI         = 0x06000000              (size 0x480000)
      app_base    = DBI + 0x00400000        => 0x06400000
                                              (Allwinner-specific glue:
                                               LTSSM_CTRL, INT_ENABLE,
                                               LINK_STAT, MISC, etc.)

  Standard DesignWare CSRs live at low DBI offsets (0x000-0x9ff:
  PORT_LINK_CONTROL=0x710, MISC_CONTROL_1_CFG=0x8bc, ATU=0x300000+).

  Allwinner glue (offsets are relative to app_base = DBI+0x400000):
      0x200  AWMISC_CTRL
      0x220  ARMISC_CTRL
      0x800  PHY_CFG
      0xc00  LTSSM_CTRL          (BIT(0) = device-type RC/EP, ... BIT(?) = LTSSM_EN)
      0xe04  INT_ENABLE_CLR      (RDLH_LINK_MASK / SMLH_LINK_MASK)
      0xe0c  LINK_STAT           (BIT(1) RDLH_LINK | BIT(0) SMLH_LINK)

  RC config / iATU outbound (from snapshot + DT range):
      0x22000000-0x27ffffff      cpu-side window for PCI MEM/IO/CFG
      0x22100000                 NVMe endpoint BAR0 (16 KB, BSP iomem)
      0x22200000                 RC root-port config space (iATU CFG window)

  BSP bring-up sequence (see sunxi_pcie_plat_hw_init + host_link_up):
      1. Power: regulator_enable(pcie3v3, pcie1v8)
      2. Resets: deassert pclk_rst (RST_BUS_PCIE0),
                 pwrup_rst (RST_BUS_PCIE0_PWRUP),
                 its_rst   (RST_BUS_ITS_PCIE0)
      3. Clocks: enable pclk_aux (CLK_PCIE0_AUX),
                 pclk_slv (CLK_PCIE0_AXI_SLV) @ 400 MHz,
                 its      (CLK_ITS_PCIE0_A)
      4. PHY:    phy_init() on combo1_pcie (cadence-combophy at 0x06c02000
                 + 0x06ca0000)
      5. Set RC mode in app_base+LTSSM_CTRL
      6. Enable LTSSM training in app_base+LTSSM_CTRL
      7. Wait for app_base+LINK_STAT bits set (link up)
      8. Program iATU outbound windows at DBI+0x300000+

  Where we are (build #41 result, 2026-05-14)
  -------------------------------------------
  Diagnostic dump in build #41 proved the link survives bootm:
      app  @0x06400000 LTSSM=0x00000041 INT_EN_CLR=0x00000000 LINK_STAT=0x00000013
      DBI  @0x06000000 PORT_LINK=0x00010120 LINK_SPEED=0x00000178 MISC1=0x00000040
      RC   @0x22200000 VID|DID=0x0A013FFF  RevID|Class=0x00010400
  - LTSSM bit0 still set, LINK_STAT bits 0+1 set (SMLH+RDLH up).
  - DBI core registers are sane.
  - RC bridge config (Class=0x0604 PCI-PCI bridge) reachable.
  - Endpoint @0x22100000 still 0xFF.

  Conclusion: U-Boot's PCIe driver `.remove` only invalidated the
  iATU outbound windows (DesignWare core does this on disable). The
  link, clocks, PHY, and PCIE_USER glue all stayed up. We just need
  to re-program iATU and the NVMe BAR.

  Build #42 strategy:
    1. Program iATU OB region 0 = CFG type-0 → bus 1 dev 0,
       cpu_addr=0x22200000, size=1MB.
    2. Read NVMe CFG space at 0x22200000 to confirm bridge traversal.
    3. Set NVMe BAR0=0x22100000 (in CFG space) + iATU OB region 1 =
       MEM identity-map cpu=pci=0x22100000, size=64KB.
    4. Enable PCI_COMMAND.{Memory,Master} on the NVMe.
    5. RegisterNonDiscoverableMmioDevice (existing code).

  Live values cross-checked against:
    research/sun60iw2-pcie-dbi-snapshot.txt   (NVMe BAR0 head dump,
                                               Linux state — misleading
                                               for entry-state)
    research/sun60iw2-iomem.txt               (Linux /proc/iomem)
        06000000-0647ffff : 6000000.pcie dbi
        22000000-27ffffff : pcie@6000000
          22100000-221fffff : PCI Bus 0000:01
            22100000-22103fff : 0000:01:00.0
              22100000-22103fff : nvme

  BSP source consulted (sparse-checked-out to /home/jacob/bsp-ref/
  orange-pi-5.15-sun60iw2/, gitee orangepi-xunlong):
    bsp/drivers/pcie/pcie-sunxi-{rc,plat,ep}.c, pcie-sunxi.h
    bsp/configs/linux-5.15/sun60iw2p1.dtsi  (pcie@6000000, combo1_pcie)
    bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.{c,h}  (clock IDs)
    bsp/drivers/phy/sunxi-cadence-combophy.c     (combo PHY init)

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

//
// PCIe register windows (per BSP DT pcie@6000000 + driver header)
//
#define A733_PCIE_DBI_BASE         0x06000000ULL
#define A733_PCIE_APP_BASE         0x06400000ULL   // DBI + PCIE_USER_DEFINED_REGISTER (0x400000)
#define A733_PCIE_RC_CFG_BASE      0x22200000ULL   // iATU CFG outbound window

// DBI standard DesignWare offsets
#define DBI_PORT_LINK_CONTROL      0x710
#define DBI_LINK_WIDTH_SPEED_CTRL  0x80C
#define DBI_MISC_CONTROL_1_CFG     0x8BC
#define DBI_MISC1_DBI_RO_WR_EN     BIT0

// app_base Allwinner glue offsets
#define APP_LTSSM_CTRL             0xC00
#define APP_INT_ENABLE_CLR         0xE04
#define APP_LINK_STAT              0xE0C
#define APP_LINK_SMLH_UP           BIT0
#define APP_LINK_RDLH_UP           BIT1

//
// DesignWare iATU outbound region registers (per-region stride 0x200,
// base 0x300000 inside DBI). See bsp/drivers/pcie/pcie-sunxi.h.
//
#define ATU_BASE                   0x300000
#define ATU_OUTBOUND_STRIDE        0x200
#define ATU_OB_LOWER_BASE          0x08
#define ATU_OB_UPPER_BASE          0x0C
#define ATU_OB_LIMIT               0x10
#define ATU_OB_LOWER_TARGET        0x14
#define ATU_OB_UPPER_TARGET        0x18
#define ATU_OB_CR1                 0x00
#define ATU_OB_CR2                 0x04
#define ATU_TYPE_MEM               0x0
#define ATU_TYPE_IO                0x2
#define ATU_TYPE_CFG0              0x4
#define ATU_TYPE_CFG1              0x5
#define ATU_CR2_REGION_ENABLE      BIT31

//
// iATU window assignment
//
#define ATU_INDEX_CFG              0
#define ATU_INDEX_MEM              1
#define A733_PCIE_CFG_BASE         0x22200000ULL  // CPU window for type-0 CFG TLPs
#define A733_PCIE_CFG_SIZE         0x00100000ULL  // 1 MB - covers up to 256 functions

//
// Standard PCI config space offsets (NVMe @ bus 1 dev 0 fn 0)
//
#define PCI_CFG_VENDOR_ID          0x00
#define PCI_CFG_DEVICE_ID          0x02
#define PCI_CFG_COMMAND            0x04
#define PCI_CFG_STATUS             0x06
#define PCI_CFG_CLASS_REVISION     0x08
#define PCI_CFG_BAR0               0x10
#define PCI_CFG_BAR1               0x14
#define PCI_CMD_IO_SPACE           BIT0
#define PCI_CMD_MEM_SPACE          BIT1
#define PCI_CMD_BUS_MASTER         BIT2
#define PCI_CMD_SERR_ENABLE        BIT8

//
// NVMe controller MMIO (BAR0 — programmed by BSP U-Boot if link is up).
// 16 KB matches /proc/iomem on a running BSP Linux kernel.
//
#define A733_NVME_BAR0_BASE   0x22100000ULL
#define A733_NVME_BAR0_SIZE   0x00004000ULL

//
// NVMe controller register offsets (NVMe 1.4 spec, base spec section 3.1)
//
#define NVME_REG_CAP_LO       0x00   // Controller Capabilities, low 32 bits
#define NVME_REG_VS           0x08   // Version

EFI_STATUS
EFIAPI
SunxiPcieDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      CapLo;
  UINT32      Vers;
  UINT32      AppLtssm, AppIntEn, AppLinkStat;
  UINT32      DbiPortLink, DbiLinkSpeed, DbiMisc;
  UINT32      RcCfgVendor, RcCfgClass;
  UINTN       AtuCfg, AtuMem;
  UINT32      NvmeVendor, NvmeClass, NvmeBar0, NvmeBar1, NvmeCmdSts;

  //
  // === STEP 0: DIAGNOSTIC DUMP (entry state) ===
  //
  AppLtssm     = MmioRead32 (A733_PCIE_APP_BASE + APP_LTSSM_CTRL);
  AppIntEn     = MmioRead32 (A733_PCIE_APP_BASE + APP_INT_ENABLE_CLR);
  AppLinkStat  = MmioRead32 (A733_PCIE_APP_BASE + APP_LINK_STAT);

  DbiPortLink  = MmioRead32 (A733_PCIE_DBI_BASE + DBI_PORT_LINK_CONTROL);
  DbiLinkSpeed = MmioRead32 (A733_PCIE_DBI_BASE + DBI_LINK_WIDTH_SPEED_CTRL);
  DbiMisc      = MmioRead32 (A733_PCIE_DBI_BASE + DBI_MISC_CONTROL_1_CFG);

  RcCfgVendor  = MmioRead32 (A733_PCIE_RC_CFG_BASE + 0x00);
  RcCfgClass   = MmioRead32 (A733_PCIE_RC_CFG_BASE + 0x08);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcieDxe: ENTRY diag\n"
    "  app @0x06400000 LTSSM=0x%08x INT_EN_CLR=0x%08x LINK_STAT=0x%08x\n"
    "  DBI @0x06000000 PORT_LINK=0x%08x LINK_SPEED=0x%08x MISC1=0x%08x\n"
    "  RC  @0x22200000 VID|DID=0x%08x  RevID|Class=0x%08x\n",
    AppLtssm, AppIntEn, AppLinkStat,
    DbiPortLink, DbiLinkSpeed, DbiMisc,
    RcCfgVendor, RcCfgClass
    ));

  //
  // Bail early if the link isn't actually up. (BSP U-Boot already
  // did the heavy lifting; if its run failed we have nothing to fix.)
  //
  if ((AppLinkStat & (APP_LINK_SMLH_UP | APP_LINK_RDLH_UP))
       != (APP_LINK_SMLH_UP | APP_LINK_RDLH_UP)) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcieDxe: link not up (LINK_STAT=0x%08x) - skipping NVMe bring-up\n",
      AppLinkStat
      ));
    return EFI_SUCCESS;
  }

  //
  // === STEP 1: Program iATU outbound CFG window ===
  // Maps CPU 0x22200000 + 1MB → PCIe TLP type-0 CFG to bus 1 dev 0 fn 0.
  // The 'busdev' value goes in LOWER_TARGET as bits [31:16] = bus, [15:11] = dev.
  // For bus=1, dev=0, fn=0: lower_target = 0x01000000.
  //
  AtuCfg = (UINTN)(A733_PCIE_DBI_BASE + ATU_BASE
                   + ATU_INDEX_CFG * ATU_OUTBOUND_STRIDE);

  MmioWrite32 (AtuCfg + ATU_OB_LOWER_BASE,   (UINT32)A733_PCIE_CFG_BASE);
  MmioWrite32 (AtuCfg + ATU_OB_UPPER_BASE,   0);
  MmioWrite32 (AtuCfg + ATU_OB_LIMIT,        (UINT32)(A733_PCIE_CFG_BASE + A733_PCIE_CFG_SIZE - 1));
  MmioWrite32 (AtuCfg + ATU_OB_LOWER_TARGET, 0x01000000); // bus=1 dev=0 fn=0
  MmioWrite32 (AtuCfg + ATU_OB_UPPER_TARGET, 0);
  MmioWrite32 (AtuCfg + ATU_OB_CR1,          ATU_TYPE_CFG0);
  MmioWrite32 (AtuCfg + ATU_OB_CR2,          ATU_CR2_REGION_ENABLE);

  // Verify the iATU register block is actually writable
  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcieDxe: iATU CFG @0x%lx programmed; readback CR1=0x%08x CR2=0x%08x LIMIT=0x%08x\n",
    (UINT64)AtuCfg,
    MmioRead32 (AtuCfg + ATU_OB_CR1),
    MmioRead32 (AtuCfg + ATU_OB_CR2),
    MmioRead32 (AtuCfg + ATU_OB_LIMIT)
    ));

  //
  // === STEP 2: Read NVMe CFG space ===
  // After iATU CFG is up, reads at 0x22200000 should now hit bus 1 dev 0,
  // i.e. the NVMe controller (Samsung 0x144D / WDC 0x15B7 / etc).
  //
  NvmeVendor = MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_VENDOR_ID);
  NvmeClass  = MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_CLASS_REVISION);
  NvmeBar0   = MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_BAR0);
  NvmeBar1   = MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_BAR1);
  NvmeCmdSts = MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_COMMAND);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcieDxe: NVMe CFG (bus 1 dev 0)\n"
    "  VID|DID=0x%08x Class=0x%08x Cmd|Sts=0x%08x\n"
    "  BAR0=0x%08x BAR1=0x%08x\n",
    NvmeVendor, NvmeClass, NvmeCmdSts,
    NvmeBar0, NvmeBar1
    ));

  if ((NvmeVendor == 0xFFFFFFFF) || (NvmeVendor == 0x00000000)) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcieDxe: NVMe CFG unreachable - iATU/link issue, aborting\n"
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // === STEP 3: Program NVMe BAR0 = our window, then iATU MEM ===
  // BAR0 type bits (low 4 bits of the value U-Boot wrote, captured in
  // NvmeBar0 above): 0x4 = 64-bit memory, 0x0 = 32-bit memory.
  // We preserve the type bits and set the address to A733_NVME_BAR0_BASE.
  // Then iATU MEM is identity-mapped (cpu_addr == pci_addr) so PCIe
  // address == our CPU address.
  //
  MmioWrite32 (
    A733_PCIE_RC_CFG_BASE + PCI_CFG_BAR0,
    ((UINT32)A733_NVME_BAR0_BASE & 0xFFFFFFF0) | (NvmeBar0 & 0xF)
    );
  if ((NvmeBar0 & 0x6) == 0x4) {
    // 64-bit BAR: high half = 0
    MmioWrite32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_BAR1, 0);
  }

  AtuMem = (UINTN)(A733_PCIE_DBI_BASE + ATU_BASE
                   + ATU_INDEX_MEM * ATU_OUTBOUND_STRIDE);

  MmioWrite32 (AtuMem + ATU_OB_LOWER_BASE,   (UINT32)A733_NVME_BAR0_BASE);
  MmioWrite32 (AtuMem + ATU_OB_UPPER_BASE,   0);
  MmioWrite32 (AtuMem + ATU_OB_LIMIT,        (UINT32)(A733_NVME_BAR0_BASE + A733_NVME_BAR0_SIZE - 1));
  MmioWrite32 (AtuMem + ATU_OB_LOWER_TARGET, (UINT32)A733_NVME_BAR0_BASE);
  MmioWrite32 (AtuMem + ATU_OB_UPPER_TARGET, 0);
  MmioWrite32 (AtuMem + ATU_OB_CR1,          ATU_TYPE_MEM);
  MmioWrite32 (AtuMem + ATU_OB_CR2,          ATU_CR2_REGION_ENABLE);

  //
  // === STEP 4: Enable Memory Space + Bus Master in NVMe Command ===
  //
  NvmeCmdSts = MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_COMMAND);
  NvmeCmdSts = (NvmeCmdSts & 0xFFFF0000) |
               PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_SERR_ENABLE;
  MmioWrite32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_COMMAND, NvmeCmdSts);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcieDxe: NVMe BAR0 reprogrammed to 0x%lx; Cmd|Sts now 0x%08x\n",
    A733_NVME_BAR0_BASE,
    MmioRead32 (A733_PCIE_RC_CFG_BASE + PCI_CFG_COMMAND)
    ));

  //
  // === STEP 5: Verify NVMe MMIO is now reachable ===
  //
  CapLo = MmioRead32 (A733_NVME_BAR0_BASE + NVME_REG_CAP_LO);
  Vers  = MmioRead32 (A733_NVME_BAR0_BASE + NVME_REG_VS);

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcieDxe: NVMe MMIO @ 0x%lx CAP_LO=0x%08x VS=0x%08x\n",
    A733_NVME_BAR0_BASE,
    CapLo,
    Vers
    ));

  if ((CapLo == 0xFFFFFFFF) || (CapLo == 0x00000000)) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiPcieDxe: NVMe MMIO still invalid after iATU MEM + BAR0 - aborting\n"
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // === STEP 6: Hand off to NvmExpressDxe ===
  // DMA: PCIe is dma-coherent on this SoC.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeNvme,
             NonDiscoverableDeviceDmaTypeCoherent,
             NULL,
             NULL,
             1,
             A733_NVME_BAR0_BASE,
             A733_NVME_BAR0_SIZE
             );

  DEBUG ((
    DEBUG_ERROR,
    "SunxiPcieDxe: NVMe register: %r\n",
    Status
    ));

  return Status;
}
