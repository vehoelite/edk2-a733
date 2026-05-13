/** @file
  PlatformBootManagerLib for Orange Pi 4 Pro (Allwinner A733).

  Wires the serial UART as ConOut/ConIn/ErrOut, connects all controllers,
  and registers a UEFI Shell boot option from the firmware volume.

  Copyright (c) 2024-2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SerialIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/GraphicsOutput.h>
#include <Guid/SerialPortLibVendor.h>
#include <Guid/GlobalVariable.h>

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        SerialDxe;
  UART_DEVICE_PATH          Uart;
  VENDOR_DEVICE_PATH        TermType;
  EFI_DEVICE_PATH_PROTOCOL  End;
} PLATFORM_SERIAL_CONSOLE;
#pragma pack()

#define PLATFORM_PC_ANSI_GUID \
  { 0xe0c14753, 0xf9be, 0x11d2, { 0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

STATIC PLATFORM_SERIAL_CONSOLE  mSerialConsole = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP,
      { (UINT8)sizeof (UART_DEVICE_PATH), 0 } },
    0, 115200, 8, 1, 1
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    PLATFORM_PC_ANSI_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_STATUS                 Status;
  UINTN                      HandleCount;
  EFI_HANDLE                *Handles;
  UINTN                      Index;
  EFI_DEVICE_PATH_PROTOCOL  *GopDp;

  //
  // Serial console is always present.
  //
  Dp = (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole;
  EfiBootManagerUpdateConsoleVariable (ConOut, Dp, NULL);
  EfiBootManagerUpdateConsoleVariable (ConIn,  Dp, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, Dp, NULL);

  //
  // SunxiSimpleFbGopDxe installs GOP + DevicePath on a single handle
  // before BeforeConsole runs (DXE dispatch is complete). Add that
  // handle's DevicePath to ConOut/ErrOut so ConSplitter + GraphicsConsoleDxe
  // bind a text console on top of the panel.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      Status = gBS->HandleProtocol (
                      Handles[Index],
                      &gEfiDevicePathProtocolGuid,
                      (VOID **)&GopDp
                      );
      if (!EFI_ERROR (Status) && (GopDp != NULL)) {
        EfiBootManagerUpdateConsoleVariable (ConOut, GopDp, NULL);
        EfiBootManagerUpdateConsoleVariable (ErrOut, GopDp, NULL);
      }
    }
    FreePool (Handles);
  }
}

STATIC
EFI_STATUS
RegisterFvBootOption (
  IN  EFI_GUID  *FileGuid,
  IN  CHAR16    *Description,
  IN  UINT32    Attributes
  )
{
  EFI_STATUS                         Status;
  UINTN                              HandleCount;
  EFI_HANDLE                         *Handles;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_DEVICE_PATH_PROTOCOL           *FvDp;
  EFI_DEVICE_PATH_PROTOCOL           *FullDp;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  UINTN                              Size;
  VOID                               *Buffer;
  UINT32                             AuthStatus;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             FvAttribs;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }
    Buffer    = NULL;
    Size      = 0;
    FileType  = EFI_FV_FILETYPE_APPLICATION;
    FvAttribs = 0;
    AuthStatus = 0;
    Status = Fv->ReadFile (Fv, FileGuid, &Buffer, &Size, &FileType, &FvAttribs, &AuthStatus);
    if (Buffer != NULL) {
      FreePool (Buffer);
    }
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&FvDp
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
    FullDp = AppendDevicePathNode (FvDp, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
    if (FullDp == NULL) {
      continue;
    }

    Status = EfiBootManagerInitializeLoadOption (
               &Option,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               Attributes,
               Description,
               FullDp,
               NULL,
               0
               );
    if (!EFI_ERROR (Status)) {
      EfiBootManagerAddLoadOptionVariable (&Option, (UINTN)-1);
      EfiBootManagerFreeLoadOption (&Option);
    }
    FreePool (FullDp);
    FreePool (Handles);
    return EFI_SUCCESS;
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

STATIC
VOID
DumpBootOptions (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
  UINTN                          BootOptionCount;
  UINTN                          Index;
  CHAR16                        *DpStr;

  BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);
  Print (L"Discovered %u boot option(s):\r\n", (UINT32)BootOptionCount);
  for (Index = 0; Index < BootOptionCount; Index++) {
    DpStr = ConvertDevicePathToText (BootOptions[Index].FilePath, FALSE, FALSE);
    Print (
      L"  Boot%04x  %s  ->  %s\r\n",
      BootOptions[Index].OptionNumber,
      BootOptions[Index].Description != NULL ? BootOptions[Index].Description : L"<no name>",
      DpStr != NULL ? DpStr : L"<no path>"
      );
    if (DpStr != NULL) {
      FreePool (DpStr);
    }
  }
  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_GUID  ShellGuid = { 0x7C04A583, 0x9E3E, 0x4f1c,
                          { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 } };

  Print (L"\r\nOrange Pi 4 Pro UEFI (Allwinner A733) - carpi-os edk2-a733\r\n");
  Print (L"Press ESC for Boot Manager\r\n\r\n");

  //
  // Connect everything so block I/O, FAT, USB, NVMe etc. all attach;
  // RefreshAllBootOption will then auto-create boot entries for any
  // removable media containing \EFI\BOOT\BOOTAA64.EFI (the UEFI fallback path).
  //
  EfiBootManagerConnectAll ();
  EfiBootManagerRefreshAllBootOption ();

  //
  // Always keep the embedded UEFI Shell available as a recovery option.
  //
  RegisterFvBootOption (&ShellGuid, L"UEFI Shell", LOAD_OPTION_ACTIVE);

  //
  // Print what we found so the user can see USB/SD/NVMe detection over UART.
  //
  DumpBootOptions ();
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  Print (L"Unable to boot. Dropping to UEFI shell.\r\n");
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  Print (L".");
}
