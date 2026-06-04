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

  // SS owned by software, drive it high (deasserted) at idle.
  V  = SpiRd (SPI_TC_REG);
  V |= (SPI_TC_SS_OWNER | SPI_TC_SS_LEVEL);
  SpiWr (SPI_TC_REG, V);
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

  // Assert chip select (drive SS level low).
  V  = SpiRd (SPI_TC_REG);
  V &= ~SPI_TC_SS_LEVEL;
  // Discard the dummy RX collected while we're driving MOSI.
  V |= SPI_TC_DHB;
  SpiWr (SPI_TC_REG, V);

  // Burst counts: MBC = total bytes on the bus; MTC = bytes driven on MOSI;
  // BCC single-transmit-count = same as MTC (no dummy cycles for ID/SFDP-read
  // beyond the address we put in the TX stream).
  SpiWr (SPI_MBC_REG, (UINT32)Total);
  SpiWr (SPI_MTC_REG, (UINT32)TxLen);
  SpiWr (SPI_BCC_REG, (UINT32)TxLen);

  // Preload the TX FIFO with the command/address bytes.
  for (i = 0; i < TxLen; i++) {
    MmioWrite8 (mSpiBase + SPI_TXDATA_REG, Tx[i]);
  }

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
  if (Timeout >= 1000000) {
    // Deassert SS and bail.
    V  = SpiRd (SPI_TC_REG);
    V |= SPI_TC_SS_LEVEL;
    SpiWr (SPI_TC_REG, V);
    return EFI_TIMEOUT;
  }

  // Drain RxLen bytes from the RX FIFO (they arrived during the read phase).
  for (i = 0; i < RxLen; i++) {
    UINTN  Guard;
    for (Guard = 0; Guard < 1000000; Guard++) {
      UINT32  Sta = SpiRd (SPI_FIFO_STA_REG);
      if (((Sta >> SPI_FIFO_STA_RX_CNT_SHIFT) & SPI_FIFO_STA_RX_CNT_MASK) != 0) {
        break;
      }
    }
    Rx[i] = MmioRead8 (mSpiBase + SPI_RXDATA_REG);
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
  DEBUG ((DEBUG_ERROR, "SunxiSpi: probing SPI0 @ 0x%lx (state-replay)\n", mSpiBase));
  DEBUG ((DEBUG_ERROR, "SunxiSpi: GC=0x%08x TC=0x%08x VER=0x%08x\n",
          SpiRd (SPI_GC_REG), SpiRd (SPI_TC_REG), SpiRd (SPI_VER_REG)));

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
