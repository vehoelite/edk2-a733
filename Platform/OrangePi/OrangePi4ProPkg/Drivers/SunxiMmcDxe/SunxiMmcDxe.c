/** @file
  SunxiMmcDxe — minimal EFI_BLOCK_IO driver for the Allwinner A733 SMHC0
  controller (the SD card slot, Linux mmcblk1).

  This is a deliberately small "state-replay" driver: the SD card and the
  SMHC0 controller have already been fully initialised by the BSP U-Boot that
  hands off to us (BL33). We do NOT re-run the SD identification sequence
  (CMD0/CMD8/ACMD41/CMD2/CMD3/CMD7/...) — we simply re-use the controller in
  its running state and issue block reads. That keeps the driver tiny and
  avoids re-implementing the full SD spec just to read a few files from /boot.

  Reads use PIO (CPU reads the 32-bit FIFO) rather than IDMAC, so there are no
  DMA descriptors and no cache-coherency dance — the CPU writes straight into
  the caller's buffer.

  Card facts (probed from Linux on the running board):
    - SMHC0 register base : 0x04020000
    - Capacity            : 1000243200 x 512-byte sectors (LastBlock 1000243199)
    - High-capacity (SDXC): block-addressed, so CMD arg == LBA

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>

//
// SMHC0 controller — the SD card slot on the Orange Pi 4 Pro (A733).
//
#define SMHC0_BASE              0x04020000ULL

//
// SMHC register offsets (from the Allwinner SMHC block, cross-checked against
// the SyterKit reg-smhc.h port).
//
#define SMHC_GCTRL              0x00    // global control
#define SMHC_CLKCR              0x04    // clock control
#define SMHC_TMOUT              0x08    // timeout
#define SMHC_WIDTH              0x0C    // bus width
#define SMHC_BLKSZ              0x10    // block size
#define SMHC_BYTECNT            0x14    // byte count
#define SMHC_CMDR               0x18    // command register
#define SMHC_CARG               0x1C    // command argument
#define SMHC_RESP0              0x20
#define SMHC_RESP1              0x24
#define SMHC_RESP2              0x28
#define SMHC_RESP3              0x2C
#define SMHC_IMASK              0x30    // interrupt mask
#define SMHC_MISTA              0x34    // masked interrupt status
#define SMHC_RINT               0x38    // raw interrupt status
#define SMHC_STATUS             0x3C    // status
#define SMHC_FIFO               0x200   // PIO data FIFO window

//
// GCTRL bits
//
#define SMHC_GCTRL_SOFT_RESET   (1U << 0)
#define SMHC_GCTRL_FIFO_RESET   (1U << 1)
#define SMHC_GCTRL_DMA_RESET    (1U << 2)
#define SMHC_GCTRL_ACCESS_BY_AHB (1U << 31)   // PIO mode: FIFO via AHB
#define SMHC_GCTRL_HW_RESET     (SMHC_GCTRL_SOFT_RESET | \
                                 SMHC_GCTRL_FIFO_RESET | \
                                 SMHC_GCTRL_DMA_RESET)

//
// CMDR bits
//
#define SMHC_CMD_RESP_EXPIRE    (1U << 6)
#define SMHC_CMD_LONG_RESPONSE  (1U << 7)
#define SMHC_CMD_CHECK_CRC      (1U << 8)
#define SMHC_CMD_DATA_EXPIRE    (1U << 9)
#define SMHC_CMD_WRITE          (1U << 10)    // 0 = read, 1 = write
#define SMHC_CMD_AUTO_STOP      (1U << 12)    // controller issues CMD12
#define SMHC_CMD_WAIT_PRE_OVER  (1U << 13)
#define SMHC_CMD_START          (1U << 31)

//
// RINT (raw interrupt) bits
//
#define SMHC_RINT_RESP_ERROR        (1U << 1)
#define SMHC_RINT_COMMAND_DONE      (1U << 2)
#define SMHC_RINT_DATA_OVER         (1U << 3)
#define SMHC_RINT_TX_REQUEST        (1U << 4)
#define SMHC_RINT_RX_REQUEST        (1U << 5)
#define SMHC_RINT_RESP_CRC_ERROR    (1U << 6)
#define SMHC_RINT_DATA_CRC_ERROR    (1U << 7)
#define SMHC_RINT_RESP_TIMEOUT      (1U << 8)
#define SMHC_RINT_DATA_TIMEOUT      (1U << 9)
#define SMHC_RINT_AUTO_CMD_DONE     (1U << 14)
// Aggregate of all the error bits worth aborting on.
#define SMHC_RINT_ERROR_MASK        0xbbc2U

//
// STATUS bits
//
#define SMHC_STATUS_FIFO_EMPTY      (1U << 2)
#define SMHC_STATUS_FIFO_FULL       (1U << 3)
#define SMHC_STATUS_CARD_BUSY       (1U << 9)
#define SMHC_STATUS_FIFO_LEVEL(x)   (((x) >> 17) & 0x3FFF)

//
// SD commands we use.
//
#define MMC_CMD_READ_SINGLE_BLOCK   17
#define MMC_CMD_READ_MULTIPLE_BLOCK 18

#define MMC_BLOCK_SIZE              512

//
// Card geometry (probed on the running board).
//
#define SD_LAST_BLOCK              (1000243200ULL - 1ULL)

//
// Bound a single CMD18 burst so BYTECNT and the PIO loop stay sane.
//
#define MAX_BLOCKS_PER_XFER        1024

//
// Spin budgets. These are raw busy-loop counts, not time — generous enough to
// cover a slow card without wedging the boot if something is wrong.
//
#define FIFO_SPIN_LIMIT            10000000U
#define DONE_SPIN_LIMIT            10000000U

#define SUNXI_MMC_SIGNATURE        SIGNATURE_32 ('s', 'm', 'm', 'c')

typedef struct {
  UINT32                      Signature;
  EFI_HANDLE                  Handle;
  EFI_BLOCK_IO_PROTOCOL       BlockIo;
  EFI_BLOCK_IO_MEDIA          Media;
  UINTN                       RegBase;
} SUNXI_MMC_PRIVATE;

#define SUNXI_MMC_FROM_BLOCKIO(a) \
  BASE_CR (a, SUNXI_MMC_PRIVATE, BlockIo)

//
// A vendor-defined device path so PartitionDxe / the FS stack can hang off us.
//
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SUNXI_MMC_DEVICE_PATH;
#pragma pack()

// {7B9C3E1A-2D4F-4A6B-9C8E-1F0A3B5C7D9E}
#define SUNXI_MMC_DP_GUID \
  { 0x7b9c3e1a, 0x2d4f, 0x4a6b, { 0x9c, 0x8e, 0x1f, 0x0a, 0x3b, 0x5c, 0x7d, 0x9e } }

STATIC SUNXI_MMC_DEVICE_PATH mSunxiMmcDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) } },
    SUNXI_MMC_DP_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC
UINT32
MmcRead (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINTN              Offset
  )
{
  return MmioRead32 (Priv->RegBase + Offset);
}

STATIC
VOID
MmcWrite (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINTN              Offset,
  IN UINT32             Value
  )
{
  MmioWrite32 (Priv->RegBase + Offset, Value);
}

/**
  Read up to MAX_BLOCKS_PER_XFER contiguous blocks via PIO.

  Assumes the controller/card are already initialised and idle. Uses CMD17 for
  a single block and CMD18 (with hardware auto-stop) for a burst.
**/
STATIC
EFI_STATUS
MmcReadBurst (
  IN  SUNXI_MMC_PRIVATE  *Priv,
  IN  UINT64             Lba,
  IN  UINTN              BlockCount,
  OUT UINT32             *Buffer
  )
{
  UINT32  Cmd;
  UINT32  Rint;
  UINT32  Status;
  UINTN   TotalWords;
  UINTN   Word;
  UINT32  Spin;
  BOOLEAN Multi;

  if (BlockCount == 0 || BlockCount > MAX_BLOCKS_PER_XFER) {
    return EFI_INVALID_PARAMETER;
  }
  Multi      = (BOOLEAN)(BlockCount > 1);
  TotalWords = (BlockCount * MMC_BLOCK_SIZE) / sizeof (UINT32);

  //
  // Make sure the FIFO is empty and switch to AHB/PIO access for the data path.
  //
  MmcWrite (Priv, SMHC_GCTRL,
    MmcRead (Priv, SMHC_GCTRL) | SMHC_GCTRL_FIFO_RESET);
  Spin = FIFO_SPIN_LIMIT;
  while ((MmcRead (Priv, SMHC_GCTRL) & SMHC_GCTRL_FIFO_RESET) != 0 &&
         (--Spin) != 0) {
  }
  MmcWrite (Priv, SMHC_GCTRL,
    MmcRead (Priv, SMHC_GCTRL) | SMHC_GCTRL_ACCESS_BY_AHB);

  //
  // Program the transfer and clear any stale interrupt bits.
  //
  MmcWrite (Priv, SMHC_BLKSZ, MMC_BLOCK_SIZE);
  MmcWrite (Priv, SMHC_BYTECNT, (UINT32)(BlockCount * MMC_BLOCK_SIZE));
  MmcWrite (Priv, SMHC_RINT, 0xFFFFFFFF);

  //
  // Issue the read command. Argument is the LBA (high-capacity card).
  //
  MmcWrite (Priv, SMHC_CARG, (UINT32)Lba);

  Cmd = (Multi ? MMC_CMD_READ_MULTIPLE_BLOCK : MMC_CMD_READ_SINGLE_BLOCK)
        | SMHC_CMD_RESP_EXPIRE
        | SMHC_CMD_CHECK_CRC
        | SMHC_CMD_DATA_EXPIRE
        | SMHC_CMD_WAIT_PRE_OVER
        | SMHC_CMD_START;
  if (Multi) {
    Cmd |= SMHC_CMD_AUTO_STOP;
  }
  MmcWrite (Priv, SMHC_CMDR, Cmd);

  //
  // Drain the FIFO word by word. The card streams data as soon as the command
  // is accepted; we wait for each word to appear (FIFO not empty) and bail on
  // any error bit.
  //
  for (Word = 0; Word < TotalWords; Word++) {
    Spin = FIFO_SPIN_LIMIT;
    for (;;) {
      Status = MmcRead (Priv, SMHC_STATUS);
      if ((Status & SMHC_STATUS_FIFO_EMPTY) == 0) {
        break;  // data available
      }
      Rint = MmcRead (Priv, SMHC_RINT);
      if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
        DEBUG ((DEBUG_ERROR,
          "SunxiMmc: read error LBA %lu RINT=0x%08x (draining)\n",
          Lba, Rint));
        return EFI_DEVICE_ERROR;
      }
      if ((--Spin) == 0) {
        DEBUG ((DEBUG_ERROR,
          "SunxiMmc: FIFO timeout LBA %lu word %u/%u STATUS=0x%08x\n",
          Lba, (UINT32)Word, (UINT32)TotalWords, Status));
        return EFI_TIMEOUT;
      }
    }
    Buffer[Word] = MmcRead (Priv, SMHC_FIFO);
  }

  //
  // Wait for the data-complete (and auto-stop, for multi-block) interrupts.
  //
  Spin = DONE_SPIN_LIMIT;
  for (;;) {
    Rint = MmcRead (Priv, SMHC_RINT);
    if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
      DEBUG ((DEBUG_ERROR,
        "SunxiMmc: post-read error LBA %lu RINT=0x%08x\n", Lba, Rint));
      return EFI_DEVICE_ERROR;
    }
    if ((Rint & SMHC_RINT_DATA_OVER) != 0) {
      if (!Multi || (Rint & SMHC_RINT_AUTO_CMD_DONE) != 0) {
        break;
      }
    }
    if ((--Spin) == 0) {
      DEBUG ((DEBUG_ERROR,
        "SunxiMmc: completion timeout LBA %lu RINT=0x%08x\n", Lba, Rint));
      return EFI_TIMEOUT;
    }
  }

  return EFI_SUCCESS;
}

/**
  EFI_BLOCK_IO_PROTOCOL.ReadBlocks — chunk the request into bursts.
**/
STATIC
EFI_STATUS
EFIAPI
SunxiMmcReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  SUNXI_MMC_PRIVATE  *Priv;
  EFI_STATUS         Status;
  UINTN              Blocks;
  UINTN              Chunk;
  UINT8              *Ptr;

  Priv = SUNXI_MMC_FROM_BLOCKIO (This);

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % MMC_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (MediaId != Priv->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  Blocks = BufferSize / MMC_BLOCK_SIZE;
  if (Lba > Priv->Media.LastBlock ||
      (Lba + Blocks - 1) > Priv->Media.LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  Ptr = (UINT8 *)Buffer;
  while (Blocks > 0) {
    Chunk = (Blocks > MAX_BLOCKS_PER_XFER) ? MAX_BLOCKS_PER_XFER : Blocks;
    Status = MmcReadBurst (Priv, (UINT64)Lba, Chunk, (UINT32 *)Ptr);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Lba    += Chunk;
    Ptr    += Chunk * MMC_BLOCK_SIZE;
    Blocks -= Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  // Read-only driver — boot files are never written from firmware.
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  // Controller is left in U-Boot's initialised state; nothing to do.
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SunxiMmcDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS         Status;
  SUNXI_MMC_PRIVATE  *Priv;
  UINT8              *Probe;

  // NOTE: this port runs with PcdDebugPrintErrorLevel = 0x80000000, so ONLY
  // DEBUG_ERROR survives to the serial console. Bring-up/self-test lines use
  // DEBUG_ERROR deliberately (same convention as SunxiUsbDxe) so they show.
  DEBUG ((DEBUG_ERROR, "SunxiMmc: init (SMHC0 @ 0x%lx, %lu sectors)\n",
    (UINT64)SMHC0_BASE, (UINT64)(SD_LAST_BLOCK + 1)));

  Priv = AllocateZeroPool (sizeof (SUNXI_MMC_PRIVATE));
  if (Priv == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Priv->Signature = SUNXI_MMC_SIGNATURE;
  Priv->RegBase   = (UINTN)SMHC0_BASE;

  Priv->Media.MediaId          = 0;
  Priv->Media.RemovableMedia   = TRUE;
  Priv->Media.MediaPresent     = TRUE;
  Priv->Media.LogicalPartition = FALSE;
  Priv->Media.ReadOnly         = TRUE;
  Priv->Media.WriteCaching     = FALSE;
  Priv->Media.BlockSize        = MMC_BLOCK_SIZE;
  Priv->Media.IoAlign          = 4;
  Priv->Media.LastBlock        = SD_LAST_BLOCK;

  Priv->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Priv->BlockIo.Media       = &Priv->Media;
  Priv->BlockIo.Reset       = SunxiMmcReset;
  Priv->BlockIo.ReadBlocks  = SunxiMmcReadBlocks;
  Priv->BlockIo.WriteBlocks = SunxiMmcWriteBlocks;
  Priv->BlockIo.FlushBlocks = SunxiMmcFlushBlocks;

  //
  // Self-test: read LBA 0 and confirm the MBR boot signature (0x55AA).
  // This proves we can actually pull data off the SD before we install the
  // protocol the rest of the boot path will rely on.
  //
  Probe = AllocateZeroPool (MMC_BLOCK_SIZE);
  if (Probe == NULL) {
    FreePool (Priv);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = MmcReadBurst (Priv, 0, 1, (UINT32 *)Probe);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: block-0 self-test FAILED: %r\n", Status));
    FreePool (Probe);
    FreePool (Priv);
    return Status;
  }
  DEBUG ((DEBUG_ERROR,
    "SunxiMmc: block-0 read OK — MBR sig=0x%02x%02x (expect 0x55AA), "
    "bytes[0..3]=%02x %02x %02x %02x\n",
    Probe[511], Probe[510], Probe[0], Probe[1], Probe[2], Probe[3]));
  if (!(Probe[510] == 0x55 && Probe[511] == 0xAA)) {
    DEBUG ((DEBUG_ERROR,
      "SunxiMmc: MBR signature mismatch — continuing anyway\n"));
  }
  FreePool (Probe);

  //
  // Publish BlockIo + a device path so PartitionDxe can enumerate p1.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Priv->Handle,
                  &gEfiDevicePathProtocolGuid, &mSunxiMmcDevicePath,
                  &gEfiBlockIoProtocolGuid,    &Priv->BlockIo,
                  NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: InstallMultipleProtocolInterfaces: %r\n",
      Status));
    FreePool (Priv);
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "SunxiMmc: BlockIo installed (handle %p)\n",
    Priv->Handle));
  return EFI_SUCCESS;
}
