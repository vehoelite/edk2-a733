/** @file
  BootDebian — native EDK2 hand-off to an EFI-stub Linux kernel on USB.

  Does, in EDK2, exactly what the GRUB scaffolding did:
    1. find the USB FAT volume that holds \Image,
    2. read \board.dtb and install it as the EFI FDT configuration table
       (gFdtTableGuid) so the arm64 EFI stub finds the device tree,
    3. read \initrd and expose it via the Linux EFI_LOAD_FILE2 protocol on a
       VenMedia(LINUX_EFI_INITRD_MEDIA_GUID) device path (the stub's
       efi_load_initrd() looks for exactly this),
    4. LoadImage()/StartImage() \Image with the kernel command line in
       LoadOptions.

  EDK2 reads only USB mass storage on this board, so the kernel+initrd+dtb live
  on a FAT32 stick; the rootfs stays on the SD card (kernel mounts it post
  hand-off). This is the path that retires GRUB and the U-Boot boot.scr opt-in.

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>

#define KERNEL_FILE  L"\\Image"
#define DTB_FILE     L"\\board.dtb"
#define INITRD_FILE  L"\\initrd"

//
// Kernel command line. rootfs is on the SD (mmcblk1p1, UUID below); the kernel
// mounts it after the stub hands off. panic=10 keeps a faulting boot self-
// recovering (reboot -> boot.scr skip_edk2 -> BSP) while we shake out drivers.
//
// uart7 (0x07080000), not uart0: uart0's pins are only on the 3-pin debug
// header, which produces no output on this board. uart7 is PL6/PL7 on 40-pin
// header pins 8/10. earlycon works from the raw MMIO address regardless of DT;
// console=ttyS7 additionally needs uart@7080000 enabled in board.dtb.
//
// Two consoles on purpose. The VENDOR kernel (5.15-sun60iw2) binds ttyS7 via
// the vendor "allwinner,uart-v100" compatible. A MAINLINE kernel has no driver
// for that compatible, so it gets no ttyS7 at all and would boot completely
// silent. earlycon=uart8250,mmio32 talks to the 16550 registers directly and
// needs no DT binding, and keep_bootcon stops it being unregistered when the
// real console is (or is not) handed over. Note earlycon=sunxi-uart is a
// vendor-only name that even the vendor kernel rejects as unknown.
STATIC CHAR16  mCmdline[] =
  L"root=UUID=51bbd498-46f8-4cc2-8d08-d0f181a24362 rootwait rootfstype=ext4 "
  L"console=ttyS7,115200 earlycon=uart8250,mmio32,0x07080000 keep_bootcon "
  L"loglevel=7 panic=10";

// EFI_DTB_TABLE_GUID — the arm64 EFI stub reads the DTB from this config table.
STATIC EFI_GUID  mFdtTableGuid =
  { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VenMedia;
  EFI_DEVICE_PATH_PROTOCOL    End;
} INITRD_DEVICE_PATH;
#pragma pack()

// LINUX_EFI_INITRD_MEDIA_GUID — the stub matches a LoadFile2 handle on this DP.
STATIC INITRD_DEVICE_PATH  mInitrdDevicePath = {
  {
    { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC VOID   *mInitrdData = NULL;
STATIC UINTN   mInitrdSize = 0;

/**
  LoadFile2 callback the EFI stub uses to pull in the initrd.
**/
STATIC
EFI_STATUS
EFIAPI
InitrdLoadFile2 (
  IN     EFI_LOAD_FILE2_PROTOCOL   *This,
  IN     EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN     BOOLEAN                   BootPolicy,
  IN OUT UINTN                     *BufferSize,
  OUT    VOID                      *Buffer  OPTIONAL
  )
{
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }
  if ((BufferSize == NULL) || (mInitrdData == NULL) || (mInitrdSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Buffer == NULL) || (*BufferSize < mInitrdSize)) {
    *BufferSize = mInitrdSize;
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Buffer, mInitrdData, mInitrdSize);
  *BufferSize = mInitrdSize;
  return EFI_SUCCESS;
}

STATIC EFI_LOAD_FILE2_PROTOCOL  mInitrdLoadFile2 = { InitrdLoadFile2 };

/**
  Read a whole file from an opened volume root into pool memory.
**/
STATIC
EFI_STATUS
ReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CHAR16             *Path,
  OUT VOID               **Data,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  VOID               *Buf;
  UINTN              FileSize;

  Status = Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    File->Close (File);
    return Status;
  }
  FileSize = (UINTN)Info->FileSize;
  FreePool (Info);

  Buf = AllocatePool (FileSize);
  if (Buf == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->Read (File, &FileSize, Buf);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return Status;
  }
  *Data = Buf;
  *Size = FileSize;
  return EFI_SUCCESS;
}

/**
  Find the (USB) SimpleFileSystem volume that has \Image; return its handle and
  an opened volume root.
**/
STATIC
EFI_STATUS
FindBootVolume (
  OUT EFI_HANDLE         *FsHandle,
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                       Status;
  UINTN                            Count;
  UINTN                            Index;
  EFI_HANDLE                       *Handles;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *VolRoot;
  EFI_FILE_PROTOCOL                *Probe;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = Fs->OpenVolume (Fs, &VolRoot);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = VolRoot->Open (VolRoot, &Probe, KERNEL_FILE, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      Probe->Close (Probe);
      *FsHandle = Handles[Index];
      *Root     = VolRoot;
      FreePool (Handles);
      return EFI_SUCCESS;
    }
    VolRoot->Close (VolRoot);
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
BootDebianEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 FsHandle;
  EFI_FILE_PROTOCOL          *Root;
  VOID                       *Dtb;
  UINTN                      DtbSize;
  EFI_PHYSICAL_ADDRESS       DtbAddr;
  EFI_HANDLE                 InitrdHandle;
  EFI_DEVICE_PATH_PROTOCOL   *KernelDp;
  EFI_HANDLE                 KernelHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *KernelLi;

  Print (L"BootDebian: looking for \\Image on a USB volume...\n");
  Status = FindBootVolume (&FsHandle, &Root);
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: no USB volume with \\Image (%r)\n", Status);
    return Status;
  }

  //
  // 1. DTB -> FDT configuration table (in EfiACPIReclaimMemory so it survives
  //    ExitBootServices into the kernel).
  //
  Status = ReadFile (Root, DTB_FILE, &Dtb, &DtbSize);
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: read %s failed: %r\n", DTB_FILE, Status);
    Root->Close (Root);
    return Status;
  }
  DtbAddr = 0;
  Status  = gBS->AllocatePages (
                   AllocateAnyPages,
                   EfiACPIReclaimMemory,
                   EFI_SIZE_TO_PAGES (DtbSize),
                   &DtbAddr
                   );
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: AllocatePages(DTB) failed: %r\n", Status);
    Root->Close (Root);
    return Status;
  }
  CopyMem ((VOID *)(UINTN)DtbAddr, Dtb, DtbSize);
  FreePool (Dtb);
  Status = gBS->InstallConfigurationTable (&mFdtTableGuid, (VOID *)(UINTN)DtbAddr);
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: InstallConfigurationTable(FDT) failed: %r\n", Status);
    Root->Close (Root);
    return Status;
  }
  Print (L"BootDebian: FDT installed @ 0x%lx (%u bytes)\n", DtbAddr, (UINT32)DtbSize);

  //
  // 2. initrd -> Linux LoadFile2 on VenMedia(LINUX_EFI_INITRD_MEDIA_GUID).
  //
  Status = ReadFile (Root, INITRD_FILE, &mInitrdData, &mInitrdSize);
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: read %s failed: %r\n", INITRD_FILE, Status);
    Root->Close (Root);
    return Status;
  }
  InitrdHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &InitrdHandle,
                  &gEfiDevicePathProtocolGuid, &mInitrdDevicePath,
                  &gEfiLoadFile2ProtocolGuid,  &mInitrdLoadFile2,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: install initrd LoadFile2 failed: %r\n", Status);
    Root->Close (Root);
    return Status;
  }
  Print (L"BootDebian: initrd registered (%u bytes) via LoadFile2\n", (UINT32)mInitrdSize);

  //
  // 3. kernel -> LoadImage + cmdline + StartImage. (EndOfDxe is signaled by
  //    PlatformBootManagerLib, so loading from USB is permitted.)
  //
  KernelDp = FileDevicePath (FsHandle, KERNEL_FILE);
  if (KernelDp == NULL) {
    Print (L"BootDebian: FileDevicePath(\\Image) failed\n");
    return EFI_OUT_OF_RESOURCES;
  }
  Status = gBS->LoadImage (FALSE, ImageHandle, KernelDp, NULL, 0, &KernelHandle);
  FreePool (KernelDp);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    Print (L"BootDebian: LoadImage(\\Image) failed: %r\n", Status);
    return Status;
  }

  Status = gBS->HandleProtocol (KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&KernelLi);
  if (!EFI_ERROR (Status)) {
    KernelLi->LoadOptions     = mCmdline;
    KernelLi->LoadOptionsSize = (UINT32)((StrLen (mCmdline) + 1) * sizeof (CHAR16));
  }

  Print (L"BootDebian: starting kernel (cmdline: %s)\n", mCmdline);
  Status = gBS->StartImage (KernelHandle, NULL, NULL);

  // Only reached if the kernel image returns instead of booting.
  Print (L"BootDebian: kernel returned: %r\n", Status);
  return Status;
}
