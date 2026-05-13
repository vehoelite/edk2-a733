/** @file
  Sunxi SimpleFB GraphicsOutputProtocol driver for Allwinner A733 (Orange Pi 4 Pro).

  Exposes the framebuffer that the BSP / U-Boot has already programmed in
  the Allwinner DE3.0 display engine as an EFI_GRAPHICS_OUTPUT_PROTOCOL
  instance. Does not initialise the display engine itself.

  Phase 1 (this file): pure pass-through. Reads framebuffer base, geometry
  and pixel format from PCDs (provided by the platform DSC) and exposes
  Blt() in software via CopyMem.

  Phase 2 (TODO): scan DE3.0 mixer registers (0x05400000 region) at entry
  and auto-discover the active framebuffer instead of relying on PCDs.

  Phase 3 (TODO): full DE3.0 + TCON-TV + HDMI controller bring-up so we
  no longer depend on the previous boot stage having configured the panel.

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>

//
// Single hard-coded video mode. Once the DE3.0 scanner lands we'll
// build the mode list dynamically from the active mixer config.
//
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mMode;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;

//
// Vendor-defined device path: GOP / Sunxi / End
//
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SUNXI_GOP_DEVICE_PATH;
#pragma pack()

#define SUNXI_GOP_DP_GUID \
  { 0xA1B2C3D4, 0xE5F6, 0x4708, { 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67 } }

STATIC SUNXI_GOP_DEVICE_PATH  mGopDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    SUNXI_GOP_DP_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC
EFI_STATUS
EFIAPI
GopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
  )
{
  if ((ModeNumber != 0) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Info = AllocateCopyPool (sizeof (mModeInfo), &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  *SizeOfInfo = sizeof (mModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                         ModeNumber
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer  OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta         OPTIONAL
  )
{
  UINT32                              *Fb;
  UINT32                               PixelsPerLine;
  UINT32                               FbW;
  UINT32                               FbH;
  UINTN                                Row;
  UINT32                              *DstLine;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *SrcLine;
  UINTN                                BltDelta;
  UINTN                                FbStrideBytes;

  Fb            = (UINT32 *)(UINTN)mMode.FrameBufferBase;
  PixelsPerLine = mModeInfo.PixelsPerScanLine;
  FbStrideBytes = (UINTN)PixelsPerLine * sizeof (UINT32);
  FbW           = mModeInfo.HorizontalResolution;
  FbH           = mModeInfo.VerticalResolution;
  BltDelta      = (Delta == 0) ? Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) : Delta;

  // Bounds check on framebuffer side
  if ((DestinationX + Width  > FbW) || (DestinationY + Height > FbH)) {
    if (BltOperation != EfiBltVideoToBltBuffer) {
      return EFI_INVALID_PARAMETER;
    }
  }

  switch (BltOperation) {
    case EfiBltVideoFill:
      if (BltBuffer == NULL) {
        return EFI_INVALID_PARAMETER;
      }
      {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Px = *BltBuffer;
        // BGRA8888 packing matches EFI_GRAPHICS_OUTPUT_BLT_PIXEL bytewise
        UINT32  Word = 0xFF000000u
                       | ((UINT32)Px.Red   << 16)
                       | ((UINT32)Px.Green <<  8)
                       | ((UINT32)Px.Blue  <<  0);
        for (Row = 0; Row < Height; Row++) {
          UINTN  Col;
          DstLine = Fb + (DestinationY + Row) * PixelsPerLine + DestinationX;
          for (Col = 0; Col < Width; Col++) {
            DstLine[Col] = Word;
          }
        }
      }
      break;

    case EfiBltBufferToVideo:
      if (BltBuffer == NULL) {
        return EFI_INVALID_PARAMETER;
      }
      for (Row = 0; Row < Height; Row++) {
        DstLine = Fb + (DestinationY + Row) * PixelsPerLine + DestinationX;
        SrcLine = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
                    ((UINT8 *)BltBuffer + (SourceY + Row) * BltDelta
                     + SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        // 1:1 copy: BltPixel layout (B,G,R,Reserved) matches BGRA8888 word
        CopyMem (DstLine, SrcLine, Width * sizeof (UINT32));
      }
      break;

    case EfiBltVideoToBltBuffer:
      if (BltBuffer == NULL) {
        return EFI_INVALID_PARAMETER;
      }
      for (Row = 0; Row < Height; Row++) {
        UINT32                         *Src = Fb + (SourceY + Row) * PixelsPerLine + SourceX;
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Dst = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
                    ((UINT8 *)BltBuffer + (DestinationY + Row) * BltDelta
                     + DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        CopyMem (Dst, Src, Width * sizeof (UINT32));
      }
      break;

    case EfiBltVideoToVideo:
      // Do row-by-row copy, handling overlap by direction
      if (DestinationY <= SourceY) {
        for (Row = 0; Row < Height; Row++) {
          UINT32  *Src = Fb + (SourceY      + Row) * PixelsPerLine + SourceX;
          UINT32  *Dst = Fb + (DestinationY + Row) * PixelsPerLine + DestinationX;
          CopyMem (Dst, Src, Width * sizeof (UINT32));
        }
      } else {
        for (Row = Height; Row > 0; Row--) {
          UINT32  *Src = Fb + (SourceY      + Row - 1) * PixelsPerLine + SourceX;
          UINT32  *Dst = Fb + (DestinationY + Row - 1) * PixelsPerLine + DestinationX;
          CopyMem (Dst, Src, Width * sizeof (UINT32));
        }
      }
      break;

    default:
      return EFI_INVALID_PARAMETER;
  }

  // Ensure scanout sees writes (cached normal memory).
  if (BltOperation != EfiBltVideoToBltBuffer) {
    WriteBackInvalidateDataCacheRange (
      (VOID *)((UINTN)Fb + DestinationY * FbStrideBytes),
      Height * FbStrideBytes
      );
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SunxiSimpleFbGopEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINT64      FbBase;
  UINT64      FbSize;
  UINT32      Width;
  UINT32      Height;
  UINT32      Pitch;
  UINT32      Format;

  FbBase = PcdGet64 (PcdSunxiFramebufferBase);
  FbSize = PcdGet64 (PcdSunxiFramebufferSize);
  Width  = PcdGet32 (PcdSunxiFramebufferWidth);
  Height = PcdGet32 (PcdSunxiFramebufferHeight);
  Pitch  = PcdGet32 (PcdSunxiFramebufferPitch);
  Format = PcdGet32 (PcdSunxiFramebufferFormat);

  if ((FbBase == 0) || (Width == 0) || (Height == 0)) {
    DEBUG ((DEBUG_WARN, "SunxiSimpleFbGop: framebuffer PCDs not set, skipping\n"));
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&mModeInfo, sizeof (mModeInfo));
  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = Width;
  mModeInfo.VerticalResolution   = Height;
  mModeInfo.PixelFormat          = (EFI_GRAPHICS_PIXEL_FORMAT)Format;
  mModeInfo.PixelsPerScanLine    = (Pitch != 0) ? Pitch / 4 : Width;

  mMode.MaxMode         = 1;
  mMode.Mode            = 0;
  mMode.Info            = &mModeInfo;
  mMode.SizeOfInfo      = sizeof (mModeInfo);
  mMode.FrameBufferBase = (EFI_PHYSICAL_ADDRESS)FbBase;
  mMode.FrameBufferSize = (UINTN)FbSize;

  mGop.QueryMode = GopQueryMode;
  mGop.SetMode   = GopSetMode;
  mGop.Blt       = GopBlt;
  mGop.Mode      = &mMode;

  //
  // ===== A733 DE3.0 mixer0 layer-0 framebuffer takeover =====
  //
  // Discovered live on the device by snapshotting DE3.0 register space
  // before/after Linux DRM moved scanout from one buffer to another:
  //
  //   PA 0x05100008  Mixer0 GLB_DBUFF_REG       write 1 -> apply settings
  //   PA 0x05101000  Mixer0 layer-0 control     keep BSP value (0xFF008003)
  //   PA 0x05101004  Mixer0 layer-0 size        (H-1)<<16 | (W-1)
  //   PA 0x0510100c  Mixer0 layer-0 pitch       bytes per scanline
  //   PA 0x05101018  Mixer0 layer-0 SCANOUT addr (the one we care about)
  //
  // BSP U-Boot programmed DE3.0 to scan from its own splash buffer; we
  // repoint scanout to PcdSunxiFramebufferBase, then commit via DBUFF.
  // We deliberately do NOT touch the control word (0x05101000) so we
  // inherit BSP's format (BGRA8888) and enable bits.
  //
  // We also paint a clear-screen so the takeover is visible: solid dark
  // blue. Once we confirm panel updates, this paint can be removed and
  // the buffer left zeroed for the OS loader to draw into.
  //
  {
    CONST UINTN  MIXER0_DBUFF   = 0x05100008;
    CONST UINTN  MIXER0_L0_SIZE = 0x05101004;
    CONST UINTN  MIXER0_L0_PITCH= 0x0510100c;
    CONST UINTN  MIXER0_L0_ADDR = 0x05101018;

    // Clear framebuffer to opaque black so GraphicsConsoleDxe has a
    // clean canvas. Alpha=FF is required (BSP control word in 0x05101000
    // has alpha-blend bits set; A=00 would make scanout transparent).
    {
      volatile UINT32 *Pixels    = (volatile UINT32 *)(UINTN)FbBase;
      UINTN            NumPixels = (Pitch / 4) * Height;
      UINTN            i;
      for (i = 0; i < NumPixels; i++) {
        Pixels[i] = 0xFF000000;
      }
    }
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)FbBase, FbSize);

    // Repoint DE3.0 mixer0 layer-0 to our buffer.
    MmioWrite32 (MIXER0_L0_SIZE,  ((Height - 1) << 16) | (Width - 1));
    MmioWrite32 (MIXER0_L0_PITCH, Pitch);
    MmioWrite32 (MIXER0_L0_ADDR,  (UINT32)FbBase);

    // Apply via double-buffer commit.
    MmioWrite32 (MIXER0_DBUFF, 1);
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,    &mGopDevicePath,
                  &gEfiGraphicsOutputProtocolGuid, &mGop,
                  NULL
                  );

  DEBUG ((DEBUG_INFO, "SunxiSimpleFbGop: %ux%u @ 0x%lx (status=%r)\n",
          Width, Height, FbBase, Status));
  return Status;
}
