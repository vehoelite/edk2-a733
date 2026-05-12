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
  //
  // TODO(phase2): software Blt for all four operations.
  // For phase 1 we report success but do not paint, since we have no
  // confirmation that PcdSunxiFramebufferBase is correct yet.
  //
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
