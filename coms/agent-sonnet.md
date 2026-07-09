# Agent-Sonnet Work Log
# Model: claude-sonnet-4-6
# Task: PCIe/NVMe bring-up (SunxiPcieDxe)

---

## [2026-05-14 18:18 UTC] Session start

### Plan
1. Read BSP pcie-sunxi-plat.c + pcie-sunxi-rc.c for CCU clock/reset sequence
2. Read ccu-sun60iw2.h for exact register offsets for PCIE clocks/resets
3. Read sunxi-cadence-combophy.c for PHY init sequence
4. Implement SunxiPcieCcuInit() + SunxiPciePhyInit() + SunxiPcieLinkUp()
5. Build, deploy, read UART, iterate

### Key unknowns to resolve from BSP
- Exact CCU register offsets for: CLK_PCIE0_AUX, CLK_PCIE0_AXI_SLV, CLK_ITS_PCIE0_A
- Exact CCU register offsets for: RST_BUS_PCIE0, RST_BUS_PCIE0_PWRUP, RST_BUS_ITS_PCIE0
- Power domain ID for SUN60IW2_PCK_PCIE (PCK600 domain)
- Cadence Combo PHY init register sequence

Reading BSP source now...
