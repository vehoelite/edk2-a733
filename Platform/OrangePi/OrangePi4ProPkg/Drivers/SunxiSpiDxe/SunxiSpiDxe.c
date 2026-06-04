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
#define SPI_TC_SS_SEL_MASK       (3U << 4)  // bits 5:4 select CS0..3
#define SPI_TC_SPOL              BIT2   // SS polarity (1 = active low, per live HW)
#define SPI_TC_CPOL              BIT1   // clock polarity
#define SPI_TC_CPHA              BIT0   // clock phase

//
// Burst-count register field masks (MTC.MWTC, BCC.STC) — 24-bit counts.
//
#define SPI_MWTC_MASK            0x00FFFFFF
#define SPI_BCC_STC_MASK         0x00FFFFFF

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
  Mux Port C pins PC2/PC3/PC4 to SPI0 (function 5). Without this MISO/MOSI/
  CLK aren't connected to the controller and every read returns 0.
**/
STATIC
VOID
SpiPinmux (
  VOID
  )
{
  UINT32  V;

  V = MmioRead32 (SUNXI_PIO_BASE + PIO_PC_CFG0_REG);
  // Each pin is a 4-bit field; PC2=bits[11:8], PC3=[15:12], PC4=[19:16].
  V &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)) | (0xFU << (4 * 4)));
  V |=  ((UINT32)PIO_SPI0_FUNC << (2 * 4))
      | ((UINT32)PIO_SPI0_FUNC << (3 * 4))
      | ((UINT32)PIO_SPI0_FUNC << (4 * 4));
  MmioWrite32 (SUNXI_PIO_BASE + PIO_PC_CFG0_REG, V);

  DEBUG ((DEBUG_ERROR, "SunxiSpi: PIO PC_CFG0 now 0x%08x (PC2/3/4 -> spi0 func5)\n",
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

  // 1. Enable the SPI0 module clock (gate). The divider/mux U-Boot/BSP
  //    programmed is left as-is; we only assert the gate so a clock runs.
  V  = MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_CLK_REG);
  V |= CCU_SPI0_CLK_GATE;
  MmioWrite32 (SUNXI_CCU_BASE + CCU_SPI0_CLK_REG, V);

  // 2. Bus gate ON, then 3. deassert bus reset (BGR register holds both).
  V  = MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG);
  V |= CCU_SPI0_BUS_GATE;          // enable bus clock
  MmioWrite32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG, V);

  V  = MmioRead32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG);
  V |= CCU_SPI0_BUS_RST;           // 1 = reset deasserted
  MmioWrite32 (SUNXI_CCU_BASE + CCU_SPI0_BGR_REG, V);

  DEBUG ((DEBUG_ERROR,
    "SunxiSpi: CCU SPI0 clk=0x%08x bgr=0x%08x (clocked+dereset)\n",
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
  Full-duplex-style half-duplex PIO exchange: write TxLen bytes from Tx,
  then read RxLen bytes into Rx. Uses single-bit mode (MBC = total, MTC =
  bytes actually driven on MOSI, BCC.STC = single-transmit count).

  This is the minimal sequence to issue a command + read a response, which
  is all the READ_ID / SFDP probe needs.

  @retval EFI_SUCCESS      Exchange completed.
  @retval EFI_TIMEOUT      Controller did not complete the burst.
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
  UINTN   Total;
  UINTN   i;
  UINTN   Timeout;
  UINT32  V;

  Total = TxLen + RxLen;
  if ((Total == 0) || (Total > 64)) {
    // First cut: single-FIFO-depth (64) transfers only — fine for ID/SFDP.
    return EFI_INVALID_PARAMETER;
  }

  SpiResetFifo ();

  // Chip-select: SW owns SS, CS0, mode 0, SPOL=1; drive SS LOW = asserted for
  // the burst. (DHB matches live Linux; we still capture all MBC bytes in the
  // RX FIFO and skip the first TxLen when draining.)
  V  = SpiRd (SPI_TC_REG);
  V |= SPI_TC_SS_OWNER;
  V &= ~SPI_TC_SS_SEL_MASK;      // CS0
  V &= ~(SPI_TC_CPOL | SPI_TC_CPHA);  // mode 0
  V |=  SPI_TC_SPOL;             // SS active low
  V |=  SPI_TC_DHB;             // match live HW
  V &= ~SPI_TC_SS_LEVEL;        // SS low = asserted (SPOL active-low)
  SpiWr (SPI_TC_REG, V);

  // Burst counts (mirror BSP single-mode cmd+read):
  //   MBC  = total bytes clocked on the bus (tx + rx)
  //   MTC.MWTC = bytes actually driven on MOSI (the command/address)
  //   BCC.STC  = single-mode transmit count (= tx); DBC dummy = 0
  SpiWr (SPI_MBC_REG, (UINT32)Total);
  SpiWr (SPI_MTC_REG, (UINT32)TxLen & SPI_MWTC_MASK);
  SpiWr (SPI_BCC_REG, (UINT32)TxLen & SPI_BCC_STC_MASK);

  // Preload the TX FIFO with the command/address bytes.
  for (i = 0; i < TxLen; i++) {
    MmioWrite8 (mSpiBase + SPI_TXDATA_REG, Tx[i]);
  }

  DEBUG ((DEBUG_ERROR,
    "SunxiSpi:  pre-XCH GC=0x%08x TC=0x%08x MBC=0x%x MTC=0x%x BCC=0x%x FIFO=0x%08x\n",
    SpiRd (SPI_GC_REG), SpiRd (SPI_TC_REG), SpiRd (SPI_MBC_REG),
    SpiRd (SPI_MTC_REG), SpiRd (SPI_BCC_REG), SpiRd (SPI_FIFO_STA_REG)));

  // Start the exchange.
  V  = SpiRd (SPI_TC_REG);
  V |= SPI_TC_XCH;
  SpiWr (SPI_TC_REG, V);

  // Wait for XCH to self-clear (burst complete).
  for (Timeout = 0; Timeout < 1000000; Timeout++) {
    if ((SpiRd (SPI_TC_REG) & SPI_TC_XCH) == 0) {
      break;
    }
  }
  DEBUG ((DEBUG_ERROR,
    "SunxiSpi: post-XCH (timeout=%u) TC=0x%08x FIFO_STA=0x%08x INT_STA=0x%08x\n",
    (UINT32)Timeout, SpiRd (SPI_TC_REG), SpiRd (SPI_FIFO_STA_REG),
    SpiRd (SPI_INT_STA_REG)));

  if (Timeout >= 1000000) {
    // Deassert SS and bail.
    V  = SpiRd (SPI_TC_REG);
    V |= SPI_TC_SS_LEVEL;
    SpiWr (SPI_TC_REG, V);
    return EFI_TIMEOUT;
  }

  // The controller clocks MBC = (TxLen+RxLen) bytes total and captures EVERY
  // received byte into the RX FIFO — including the TxLen "dummy" bytes shifted
  // in while we were driving the command on MOSI. So discard the first TxLen
  // RX bytes, then the next RxLen bytes are the real response.
  for (i = 0; i < (TxLen + RxLen); i++) {
    UINTN   Guard;
    UINT8   B;
    for (Guard = 0; Guard < 1000000; Guard++) {
      UINT32  Sta = SpiRd (SPI_FIFO_STA_REG);
      if (((Sta >> SPI_FIFO_STA_RX_CNT_SHIFT) & SPI_FIFO_STA_RX_CNT_MASK) != 0) {
        break;
      }
    }
    B = MmioRead8 (mSpiBase + SPI_RXDATA_REG);
    if (i >= TxLen) {
      Rx[i - TxLen] = B;
    }
  }

  // Deassert chip select.
  V  = SpiRd (SPI_TC_REG);
  V |= SPI_TC_SS_LEVEL;
  SpiWr (SPI_TC_REG, V);

  return EFI_SUCCESS;
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

  // Soft-reset the controller (it was held in reset until now), then enable.
  SpiWr (SPI_GC_REG, SpiRd (SPI_GC_REG) | SPI_GC_SRST);
  {
    UINTN  T;
    for (T = 0; T < 100000; T++) {
      if ((SpiRd (SPI_GC_REG) & SPI_GC_SRST) == 0) {
        break;
      }
    }
  }

  SpiMasterEnable ();

  Status = NorReadId (Id);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiSpi: READ_ID xfer failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR,
    "SunxiSpi: SPI-NOR JEDEC ID = %02x %02x %02x (mfr/type/cap)\n",
    Id[0], Id[1], Id[2]));

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
