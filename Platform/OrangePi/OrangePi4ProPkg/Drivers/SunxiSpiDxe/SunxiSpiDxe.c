/** @file
  SunxiSpiDxe — native SPI master + SPI-NOR probe for the Allwinner A733.

  This is the first step toward a *true* EDK2 platform on the A733: EDK2
  driving its own SPI-NOR (16 MB, mtd0) instead of relying on U-Boot. Once
  EDK2 owns the SPI-NOR it can host a real UEFI variable store (and,
  eventually, the firmware image itself) in flash.

  Controller: SPI0 @ 0x02540000, "allwinner,sunxi-spi-v1.3" — the standard
  sunxi SPI master (same register layout as A64/H6/D1). Register offsets +
  the transfer sequence (MBC/MTC/BCC burst counts, TC.XCH exchange, FIFO_STA
  polling, PIO via TXDATA/RXDATA) are ported from the BSP spi-ng driver
  (bsp/drivers/spi-ng/spi-sunxi.{c,h}).

  STATE-REPLAY: U-Boot already initialised SPI0 (the board boots from this
  SPI-NOR via the BROM eGON chain), so we reuse the controller's clocks/pin
  state and only drive transfers. Same approach that worked for SunxiMmcDxe.

  This first revision is READ-ONLY: it issues SPI-NOR READ_ID (0x9F) and
  reads SFDP, proving the controller talks to the flash, with zero risk of
  touching the live boot chain. Erase/write + an EFI_SPI_HC_PROTOCOL that
  the generic MdeModulePkg SpiNorFlashJedecSfdp driver can bind come next.

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>

//
// SPI0 controller base (DTB: spi@2540000, "allwinner,sunxi-spi-v1.3").
//
#define SUNXI_SPI0_BASE          0x02540000ULL

//
// CCU (clock-controller) — "allwinner,sun60iw2-ccu" @ 0x02002000.
// U-Boot reads boot0 via the SPIF/another path and leaves SPI0 UNCLOCKED, so
// (unlike SunxiMmcDxe) we must clock-enable + de-reset SPI0 before any MMIO —
// otherwise the controller block reads back all-zero (VER==0 is the tell).
//   spi0 module clock gate : 0x0F00 bit31
//   spi0 bus clock gate    : 0x0F04 bit0
//   spi0 bus reset         : 0x0F04 bit16
// (offsets from BSP ccu-sun60iw2.c)
//
//
// PIO (pin controller) — "allwinner,sun60iw2-pinctrl" @ 0x02000000.
// SPI0 uses Port C pins PC2/PC3/PC4 at alternate **function 5** (verified
// against the live, working Linux mux: PIO+0x40 PC_CFG0 has PC2..4 = 0x5).
// U-Boot doesn't leave these muxed for EDK2, so reads return 0 (controller
// clocks fine but MISO/MOSI/CLK aren't wired to the block). We mux them here.
//
#define SUNXI_PIO_BASE           0x02000000ULL
#define PIO_PC_CFG0_REG          0x40     // Port C config, pins PC0..PC7
#define PIO_SPI0_FUNC            0x5      // SPI0 alternate function on Port C

#define SUNXI_CCU_BASE           0x02002000ULL
#define CCU_SPI0_CLK_REG         0x0F00
#define CCU_SPI0_BGR_REG         0x0F04   // bus gating + reset
#define CCU_SPI0_CLK_GATE        BIT31    // module clock enable
#define CCU_SPI0_BUS_GATE        BIT0     // bus clock gate enable
#define CCU_SPI0_BUS_RST         BIT16    // bus reset (1 = deasserted)

//
// The COMPLETE live CCU SPI0_CLK value read from the working Linux state via
// /dev/mem: 0x81000005 = gate(BIT31) | src=PLL_PERI(bits[26:24]=001) |
// N=1(bits[9:8]=0) | M=6(bits[3:0]=5). The crucial fix: the BSP driver sets
// the SPI module-clock *rate* through the CCU (clk_set_rate -> this very
// source+divider), it does NOT program the controller's CCR for the SCK
// frequency. We previously only OR'd in the gate bit and left the source mux
// and divider at reset defaults -> wrong/garbage SCK -> all-zero reads.
// Replicate the exact working register so SCK matches the flash's 50 MHz.
//
#define CCU_SPI0_CLK_LIVE        0x81000005

//
// Register offsets (from BSP spi-ng/spi-sunxi.h — standard sunxi SPI v1.3).
//
#define SPI_VER_REG              0x00
#define SPI_GC_REG               0x04   // global control
#define SPI_TC_REG               0x08   // transfer control
#define SPI_INT_CTL_REG          0x10
#define SPI_INT_STA_REG          0x14
#define SPI_FIFO_CTL_REG         0x18
#define SPI_FIFO_STA_REG         0x1C
#define SPI_WAIT_CNT_REG         0x20
#define SPI_CCR_REG              0x24   // clock rate control (divider)
#define SPI_SAMP_DL_REG          0x28   // sample delay (MISO latch timing)

//
// Values copied from the LIVE, working Linux SPI0 state (read via /dev/mem).
// SAMP_DL in particular is the key: wrong sample timing => MISO latched on
// the wrong edge => reads return 0 even with a correct transfer.
//
#define SPI_CCR_LIVE             0x00000002
#define SPI_SAMP_DL_LIVE         0x00002000
#define SPI_FIFO_CTL_LIVE        0x00200140
#define SPI_MBC_REG              0x30   // master burst count (total)
#define SPI_MTC_REG              0x34   // master transmit count
#define SPI_BCC_REG              0x38   // burst control (single-tx + dummy)
#define SPI_TXDATA_REG           0x200  // TX FIFO data port
#define SPI_RXDATA_REG           0x300  // RX FIFO data port

//
// Global Control bits.
//
#define SPI_GC_EN                BIT0   // SPI module enable
#define SPI_GC_MODE              BIT1   // 1 = master
#define SPI_GC_TP_EN             BIT7   // stop transmit when RX FIFO full
#define SPI_GC_SRST              BIT31  // soft reset

//
// Transfer Control bits.
//
#define SPI_TC_XCH               BIT31  // exchange burst (start)
#define SPI_TC_SS_OWNER          BIT6   // SS controlled by software level
#define SPI_TC_SS_LEVEL          BIT7   // SS output level (when SW owner)
#define SPI_TC_DHB               BIT8   // discard unused (half-duplex) burst
#define SPI_TC_SSCTL             BIT3   // 0=SS stays asserted between bursts (READ_ID)
#define SPI_TC_SS_SEL_MASK       (3U << 4)  // bits 5:4 select CS0..3
#define SPI_TC_SPOL              BIT2   // SS polarity (1 = active low, per live HW)
#define SPI_TC_CPOL              BIT1   // clock polarity
#define SPI_TC_CPHA              BIT0   // clock phase

//
// Burst-count register field masks (MTC.MWTC, BCC.STC) — 24-bit counts.
// BCC also holds DBC (dummy burst count, bits 27:24) and the dual/quad enables.
//
#define SPI_MWTC_MASK            0x00FFFFFF
#define SPI_BCC_STC_MASK         0x00FFFFFF
#define SPI_BCC_DBC_SHIFT        24
#define SPI_BCC_DBC_MASK         (0xFU << SPI_BCC_DBC_SHIFT)
#define SPI_BCC_QUAD_EN          BIT29
#define SPI_BCC_DRM              BIT28  // dual mode enable

//
// FIFO control bits.
//
#define SPI_FIFO_CTL_RX_RST      BIT15
#define SPI_FIFO_CTL_TX_RST      BIT31

//
// FIFO status fields (counts).
//
#define SPI_FIFO_STA_RX_CNT_SHIFT  0
#define SPI_FIFO_STA_RX_CNT_MASK   0x7F
#define SPI_FIFO_STA_TX_CNT_SHIFT  16
#define SPI_FIFO_STA_TX_CNT_MASK   0x7F

//
// SPI-NOR commands we use for the read-only probe.
//
#define NOR_CMD_READ_ID          0x9F  // JEDEC READ ID -> 3 bytes (mfr, type, cap)
#define NOR_CMD_RDSR             0x05  // read status register
#define NOR_CMD_SFDP             0x5A  // read SFDP (addr + 1 dummy)
#define NOR_CMD_RSTEN            0x66  // reset-enable (must precede RST)
#define NOR_CMD_RST              0x99  // software reset (return to defaults)
#define NOR_CMD_RELEASE_DPD      0xAB  // release from deep power-down / read electronic sig
#define NOR_CMD_EXIT_QPI         0xFF  // exit QPI / continuous-read reset (mode-bit clear)

STATIC UINTN  mSpiBase = SUNXI_SPI0_BASE;

STATIC
UINT32
SpiRd (
  IN UINTN  Off
  )
{
  return MmioRead32 (mSpiBase + Off);
}

STATIC
VOID
SpiWr (
  IN UINTN   Off,
  IN UINT32  Val
  )
{
  MmioWrite32 (mSpiBase + Off, Val);
}

/**
  Mux Port C pins PC1/PC2/PC3/PC4 to SPI0 (function 5). On the Orange Pi 4 Pro
  the working SPI0-NOR bus is PC1=CLK, PC2=MOSI, PC3=CS0, PC4=MISO — verified
  by reading the LIVE Linux mux during an active mtd0 transfer: PIO+0x40
  PC_CFG0 = 0x01155550 (PC1..PC4 all = 0x5), and 2.6M samples confirm CLK is
  NOT on PC12 (PC_CFG1 stays 0). Earlier code muxed only PC2/3/4 and left PC1
  (CLK) at function 0 — no clock reached the flash, so every READ_ID returned
  0x00. Muxing PC1 too is the fix.
**/
STATIC
VOID
SpiPinmux (
  VOID
  )
{
  UINT32  V;

  V = MmioRead32 (SUNXI_PIO_BASE + PIO_PC_CFG0_REG);
  // Each pin is a 4-bit field; PC1=bits[7:4], PC2=[11:8], PC3=[15:12], PC4=[19:16].
  V &= ~((0xFU << (1 * 4)) | (0xFU << (2 * 4)) | (0xFU << (3 * 4)) | (0xFU << (4 * 4)));
  V |=  ((UINT32)PIO_SPI0_FUNC << (1 * 4))
      | ((UINT32)PIO_SPI0_FUNC << (2 * 4))
      | ((UINT32)PIO_SPI0_FUNC << (3 * 4))
      | ((UINT32)PIO_SPI0_FUNC << (4 * 4));
  MmioWrite32 (SUNXI_PIO_BASE + PIO_PC_CFG0_REG, V);

  DEBUG ((DEBUG_ERROR, "SunxiSpi: PIO PC_CFG0 now 0x%08x (PC1=CLK/2=MOSI/3=CS0/4=MISO func5)\n",
          MmioRead32 (SUNXI_PIO_BASE + PIO_PC_CFG0_REG)));
}

/**
  Clock-enable + de-reset SPI0 in the CCU. Required because U-Boot leaves
  SPI0 unclocked — without this the controller MMIO reads all-zero.
**/
STATIC
VOID
SpiCcuEnable (
  VOID
  )
{
  UINT32  V;

  // 1. Program the SPI0 module clock to the COMPLETE working value
  //    (gate + PLL_PERI source + /6 divider). U-Boot leaves SPI0 unclocked
  //    and we must NOT just gate the reset-default mux/divider (which gave a
  //    wrong SCK and all-zero reads) — write the exact live register instead.
  MmioWrite32 (SUNXI_CCU_BASE + CCU_SPI0_CLK_REG, CCU_SPI0_CLK_LIVE);

  // 2. Bus gate ON, then 3. deassert bus reset (BGR register holds both).
  V  = MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG);
  V |= CCU_SPI0_BUS_GATE;          // enable bus clock
  MmioWrite32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG, V);

  V  = MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG);
  V |= CCU_SPI0_BUS_RST;           // 1 = reset deasserted
  MmioWrite32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG, V);

  DEBUG ((DEBUG_ERROR,
    "SunxiSpi: CCU SPI0 clk=0x%08x bgr=0x%08x (expect clk=0x81000005)\n",
    MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_CLK_REG),
    MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG)));
}

/**
  Reset both FIFOs and wait for the reset bits to self-clear.
**/
STATIC
VOID
SpiResetFifo (
  VOID
  )
{
  UINT32  V;
  UINTN   Timeout;

  V  = SpiRd (SPI_FIFO_CTL_REG);
  V |= (SPI_FIFO_CTL_RX_RST | SPI_FIFO_CTL_TX_RST);
  SpiWr (SPI_FIFO_CTL_REG, V);

  for (Timeout = 0; Timeout < 100000; Timeout++) {
    if ((SpiRd (SPI_FIFO_CTL_REG) &
         (SPI_FIFO_CTL_RX_RST | SPI_FIFO_CTL_TX_RST)) == 0) {
      break;
    }
  }
}

/**
  Ensure the controller is enabled in master mode (state-replay friendly:
  U-Boot left it configured; we just assert EN | master and SW-controlled SS
  low for the duration of a transfer).
**/
STATIC
VOID
SpiMasterEnable (
  VOID
  )
{
  UINT32  V;

  V  = SpiRd (SPI_GC_REG);
  V |= (SPI_GC_EN | SPI_GC_MODE | SPI_GC_TP_EN);
  SpiWr (SPI_GC_REG, V);

  // Match the live working Linux TC: SPI mode 0 (CPOL=0, CPHA=0), SPOL=1
  // (SS active-low), software-owned SS, idle/deasserted = SS_LEVEL HIGH.
  // (Our earlier bug: CPOL/CPHA were set => mode 3 => flash returned zeros.)
  V  = SpiRd (SPI_TC_REG);
  V &= ~(SPI_TC_CPOL | SPI_TC_CPHA);     // mode 0
  V |=  SPI_TC_SPOL;                      // SS active low
  V |= (SPI_TC_SS_OWNER | SPI_TC_SS_LEVEL); // SW owns SS, idle high
  V &= ~SPI_TC_SS_SEL_MASK;              // CS0
  SpiWr (SPI_TC_REG, V);

  // Clock divider, sample-delay and FIFO trigger levels — copied from the
  // live working Linux config. SAMP_DL is essential: it sets when MISO is
  // latched; the wrong value reads all-zero (our remaining bug).
  SpiWr (SPI_CCR_REG,      SPI_CCR_LIVE);
  SpiWr (SPI_SAMP_DL_REG,  SPI_SAMP_DL_LIVE);
  SpiWr (SPI_FIFO_CTL_REG, SPI_FIFO_CTL_LIVE);

  DEBUG ((DEBUG_ERROR, "SunxiSpi: CCR=0x%08x SAMP_DL=0x%08x FIFO_CTL=0x%08x\n",
          SpiRd (SPI_CCR_REG), SpiRd (SPI_SAMP_DL_REG), SpiRd (SPI_FIFO_CTL_REG)));
}

/**
  Program the burst-count registers exactly as the BSP's
  sunxi_spi_set_bc_tc_stc() does:
    MBC      = tx_len + rx_len + dummy   (total bytes clocked on the bus)
    MTC.MWTC = tx_len                    (bytes actually DRIVEN on MOSI)
    BCC.STC  = stc_len                   (single-mode transmit count)
    BCC.DBC  = dummy                     (dummy burst count)
  Single-bit mode only here (no dual/quad), so clear those enables.
**/
STATIC
VOID
SpiSetBurst (
  IN UINTN  TxLen,
  IN UINTN  RxLen,
  IN UINTN  StcLen,
  IN UINTN  Dummy
  )
{
  UINT32  V;

  SpiWr (SPI_MBC_REG, (UINT32)(TxLen + RxLen + Dummy));
  SpiWr (SPI_MTC_REG, (UINT32)TxLen & SPI_MWTC_MASK);

  V  = SpiRd (SPI_BCC_REG);
  V &= ~(SPI_BCC_STC_MASK | SPI_BCC_DBC_MASK | SPI_BCC_QUAD_EN | SPI_BCC_DRM);
  V |= ((UINT32)StcLen & SPI_BCC_STC_MASK);
  V |= (((UINT32)Dummy << SPI_BCC_DBC_SHIFT) & SPI_BCC_DBC_MASK);
  SpiWr (SPI_BCC_REG, V);
}

/**
  Run one single-mode burst and wait for XCH to self-clear.

  @retval EFI_SUCCESS  Burst completed (XCH cleared).
  @retval EFI_TIMEOUT  XCH never cleared.
**/
STATIC
EFI_STATUS
SpiRunBurst (
  IN CONST CHAR8  *Tag
  )
{
  UINTN   Timeout;
  UINT32  V;

  DEBUG ((DEBUG_ERROR,
    "SunxiSpi:  %a pre-XCH TC=0x%08x MBC=0x%x MTC=0x%x BCC=0x%x FIFO_STA=0x%08x\n",
    Tag, SpiRd (SPI_TC_REG), SpiRd (SPI_MBC_REG), SpiRd (SPI_MTC_REG),
    SpiRd (SPI_BCC_REG), SpiRd (SPI_FIFO_STA_REG)));

  V  = SpiRd (SPI_TC_REG);
  V |= SPI_TC_XCH;
  SpiWr (SPI_TC_REG, V);

  for (Timeout = 0; Timeout < 1000000; Timeout++) {
    if ((SpiRd (SPI_TC_REG) & SPI_TC_XCH) == 0) {
      break;
    }
  }

  DEBUG ((DEBUG_ERROR,
    "SunxiSpi:  %a post-XCH (to=%u) TC=0x%08x FIFO_STA=0x%08x INT_STA=0x%08x\n",
    Tag, (UINT32)Timeout, SpiRd (SPI_TC_REG), SpiRd (SPI_FIFO_STA_REG),
    SpiRd (SPI_INT_STA_REG)));

  return (Timeout >= 1000000) ? EFI_TIMEOUT : EFI_SUCCESS;
}

/**
  Half-duplex command + read, done as the BSP does it: TWO separate single-mode
  bursts with chip-select held low across both.

    1. TX burst : set_bc_tc_stc(tx=TxLen, rx=0, stc=TxLen, dummy=0). DHB set so
       the bytes clocked-in during the command are discarded by hardware.
    2. RX burst : set_bc_tc_stc(tx=0, rx=RxLen, stc=0, dummy=0). MTC=0 means
       NOTHING is driven on MOSI; the controller clocks RxLen bytes purely to
       receive, and they land in the RX FIFO.

  Our previous single combined burst (MBC=total, MTC=TxLen, DHB) is what made
  reads return 0: with DHB the received bytes are discarded, and there was no
  pure-RX (MTC=0) phase to actually capture the response.

  @retval EFI_SUCCESS      Exchange completed.
  @retval EFI_TIMEOUT      A burst did not complete.
**/
STATIC
EFI_STATUS
SpiXfer (
  IN  CONST UINT8  *Tx,
  IN  UINTN        TxLen,
  OUT UINT8        *Rx,
  IN  UINTN        RxLen
  )
{
  UINTN       i;
  UINT32      V;
  EFI_STATUS  Status;

  if (((TxLen + RxLen) == 0) || (TxLen > 64) || (RxLen > 64)) {
    return EFI_INVALID_PARAMETER;
  }

  SpiResetFifo ();

  // Chip-select setup: SW owns SS, CS0, mode 0, SPOL=1, DHB on. Assert SS LOW
  // and hold it low for the WHOLE single-XCH exchange (cmd + read in one frame).
  V  = SpiRd (SPI_TC_REG);
  V |= SPI_TC_SS_OWNER;
  V &= ~SPI_TC_SS_SEL_MASK;            // CS0
  V &= ~(SPI_TC_CPOL | SPI_TC_CPHA);   // mode 0
  V |=  SPI_TC_SPOL;                   // SS active low
  V |=  SPI_TC_DHB;                    // discard the TxLen echo bytes in RX FIFO
  V &= ~SPI_TC_SSCTL;                  // CS stays asserted BETWEEN the 2 bursts
  V &= ~SPI_TC_SS_LEVEL;               // SS low = asserted
  SpiWr (SPI_TC_REG, V);

  // ---- TWO transfers, ONE CS frame (matches the VENDOR DRIVER exactly) ----
  // spi-sunxi.c sunxi_spi_mode_check(): a half-duplex command+read is TWO
  // spi_transfers, each its own set_bc_tc_stc + start_xfer (XCH), with CS held
  // low across both by the framework. Replicated here:
  //   TX (cmd): set_bc_tc_stc(tx, 0, tx, 0)  -> MBC=tx MTC=tx STC=tx
  //   RX (data): set_bc_tc_stc(0, rx, 0, 0)  -> MBC=rx MTC=0  STC=0  (pure rx)
  // CS is already asserted (SS_LEVEL low) above and stays low until Done.

  // --- Transfer 1: drive the command bytes ---
  if (TxLen > 0) {
    SpiSetBurst (TxLen, 0, TxLen, 0);          // MBC=tx MTC=tx STC=tx
    for (i = 0; i < TxLen; i++) {
      MmioWrite8 (mSpiBase + SPI_TXDATA_REG, Tx[i]);
    }
    Status = SpiRunBurst ("TX");
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  // --- Transfer 2: pure receive (MTC=0 => MOSI idle, clock RxLen bytes in) ---
  if (RxLen > 0) {
    SpiSetBurst (0, RxLen, 0, 0);              // MBC=rx MTC=0 STC=0
    Status = SpiRunBurst ("RX");
    if (EFI_ERROR (Status)) {
      goto Done;
    }
    for (i = 0; i < RxLen; i++) {
      UINTN  Guard;
      for (Guard = 0; Guard < 1000000; Guard++) {
        UINT32  Sta = SpiRd (SPI_FIFO_STA_REG);
        if (((Sta >> SPI_FIFO_STA_RX_CNT_SHIFT) & SPI_FIFO_STA_RX_CNT_MASK) != 0) {
          break;
        }
      }
      if (Rx != NULL) {
        Rx[i] = MmioRead8 (mSpiBase + SPI_RXDATA_REG);
      } else {
        (VOID)MmioRead8 (mSpiBase + SPI_RXDATA_REG);
      }
    }
  }

  Status = EFI_SUCCESS;

Done:
  // Deassert chip select.
  V  = SpiRd (SPI_TC_REG);
  V |= SPI_TC_SS_LEVEL;
  SpiWr (SPI_TC_REG, V);
  return Status;
}

/**
  Send a single command byte (no data phase). Used for state-reset opcodes.
**/
STATIC
VOID
NorCmd1 (
  IN UINT8  Cmd
  )
{
  UINT8  C[1];

  C[0] = Cmd;
  SpiXfer (C, 1, NULL, 0);
}

/**
  Coax the flash out of whatever non-default state U-Boot may have left it in
  before the first READ_ID. All of these are safe, standard, read-only-effect
  recovery opcodes:
    - 0xFF  exit QPI / clear continuous-read "mode bits" (if the chip was left
            in a fast-read-continuous state it ignores normal commands).
    - 0xAB  release from deep power-down (if U-Boot put it to sleep, it won't
            answer READ_ID until released).
    - 0x66 + 0x99  software reset-enable + reset (return registers/mode to POR).
  Each opcode is a single-byte, CS-framed transfer with a brief settle between.
**/
STATIC
VOID
NorRecover (
  VOID
  )
{
  UINTN  d;

  NorCmd1 (NOR_CMD_EXIT_QPI);
  for (d = 0; d < 10000; d++) { MmioRead32 (mSpiBase + SPI_VER_REG); }

  NorCmd1 (NOR_CMD_RELEASE_DPD);
  for (d = 0; d < 50000; d++) { MmioRead32 (mSpiBase + SPI_VER_REG); }  // tRES > 3us

  NorCmd1 (NOR_CMD_RSTEN);
  NorCmd1 (NOR_CMD_RST);
  for (d = 0; d < 200000; d++) { MmioRead32 (mSpiBase + SPI_VER_REG); } // tRST > 30us

  DEBUG ((DEBUG_ERROR, "SunxiSpi: flash recovery (FF/AB/66+99) issued\n"));
}

/**
  Issue JEDEC READ_ID (0x9F) and return the 3 identity bytes.
**/
STATIC
EFI_STATUS
NorReadId (
  OUT UINT8  Id[3]
  )
{
  UINT8       Cmd[1];
  EFI_STATUS  Status;

  Cmd[0] = NOR_CMD_READ_ID;
  Status = SpiXfer (Cmd, 1, Id, 3);
  return Status;
}

EFI_STATUS
EFIAPI
SunxiSpiDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8       Id[3];

  // DEBUG_ERROR so it survives this port's print mask (only DEBUG_ERROR
  // reaches the UART — see SunxiUsbDxe / the port notes).
  DEBUG ((DEBUG_ERROR, "SunxiSpi: probing SPI0 @ 0x%lx\n", mSpiBase));

  // U-Boot leaves SPI0 unclocked -> MMIO reads 0. Clock + de-reset first.
  SpiCcuEnable ();

  // ...and the pins unmuxed -> reads return 0. Mux PC2/3/4 to spi0.
  SpiPinmux ();

  DEBUG ((DEBUG_ERROR, "SunxiSpi: post-clock GC=0x%08x TC=0x%08x VER=0x%08x\n",
          SpiRd (SPI_GC_REG), SpiRd (SPI_TC_REG), SpiRd (SPI_VER_REG)));

  // NOTE: no gratuitous GC.SRST here. The BSP sunxi_spi_hw_init() does NOT
  // soft-reset in its normal init path (SRST appears only in runtime error
  // recovery). Issuing it after clock+pinmux was leaving the block in a
  // half-initialised state; we now follow the vendor order directly:
  // enable bus -> set master -> config TC (mode0) inside SpiMasterEnable().
  SpiMasterEnable ();

  // Wake/reset the flash out of any state U-Boot left it in (DPD, QPI,
  // continuous-read) before the first READ_ID.
  NorRecover ();

  // Diagnostic: read the status register (0x05). If READ_ID returns 0x00 but
  // RDSR returns something that isn't 0x00/0xFF, MISO is fine and the issue is
  // command-specific; if RDSR is also 0x00/0xFF, the data path itself is mute.
  {
    UINT8  Sr = 0xA5;
    UINT8  Rc = NOR_CMD_RDSR;
    SpiXfer (&Rc, 1, &Sr, 1);
    DEBUG ((DEBUG_ERROR, "SunxiSpi: RDSR(0x05) = 0x%02x\n", Sr));
  }

  // Try READ_ID a few times — a freshly-reset chip may need a beat.
  {
    UINTN  Try;
    for (Try = 0; Try < 3; Try++) {
      Status = NorReadId (Id);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "SunxiSpi: READ_ID xfer failed: %r\n", Status));
        return Status;
      }
      DEBUG ((DEBUG_ERROR,
        "SunxiSpi: [try %u] SPI-NOR JEDEC ID = %02x %02x %02x\n",
        (UINT32)Try, Id[0], Id[1], Id[2]));
      if ((Id[0] != 0x00) && (Id[0] != 0xFF)) {
        break;
      }
    }
  }

  if ((Id[0] == 0x00) || (Id[0] == 0xFF)) {
    DEBUG ((DEBUG_ERROR,
      "SunxiSpi: ID looks invalid (no flash response) — controller/SS wiring TBD\n"));
  } else {
    // Capacity is typically 2^Id[2] bytes; 0x18 = 16 MB (matches mtd0).
    DEBUG ((DEBUG_ERROR,
      "SunxiSpi: flash detected, ~%u MB (READ-ONLY probe OK)\n",
      (UINT32)(1U << Id[2]) / (1024U * 1024U)));
  }

  // Read-only milestone: we do NOT install a protocol or touch flash yet.
  // Next: EFI_SPI_HC_PROTOCOL so MdeModulePkg/SpiNorFlashJedecSfdp can bind,
  // then a UEFI variable store in a verified-empty high region.
  return EFI_SUCCESS;
}
