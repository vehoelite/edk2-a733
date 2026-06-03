/** @file
  SunxiSmbiosDxe — publish SMBIOS records for the Allwinner A733
  (Orange Pi 4 Pro). The EDK2 UiApp / FrontPage queries
  EFI_SMBIOS_PROTOCOL for system, CPU and memory info; populating these
  records makes Setup look like a real BIOS instead of falling back to
  the "Wonder Computer Model 1000Z" placeholder strings.

  Records published:
    Type 0  BIOS Information
    Type 1  System Information
    Type 2  Baseboard
    Type 3  Chassis
    Type 4  Processor (A733, 8 cores, octa-core ARMv8.2-A)
    Type 7  Cache (L1/L2 placeholder)
    Type 16 Physical Memory Array (single 6 GB array)
    Type 17 Memory Device          (single LPDDR4X stick)
    Type 19 Memory Array Mapped Address
    Type 32 System Boot Information

  Copyright (c) 2026, Orange Pi 4 Pro EDK2 port. SPDX BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define A733_MEMORY_SIZE_MB  (6 * 1024)   // 6 GB

//
// Helper: append one SMBIOS record. Each record is a fixed structure
// followed by a sequence of NUL-terminated strings then a trailing
// double-NUL. We pack everything into a temporary buffer and call
// Smbios->Add(). String indexes are 1-based (1 = first string).
//
STATIC
EFI_STATUS
AddRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN VOID                 *Header,
  IN UINTN                 HeaderLength,
  IN CONST CHAR8         **Strings,
  IN UINTN                 StringCount,
  OUT EFI_SMBIOS_HANDLE   *Handle  OPTIONAL
  )
{
  EFI_STATUS                Status;
  EFI_SMBIOS_TABLE_HEADER  *Record;
  UINTN                     TotalLen;
  UINTN                     Index;
  UINTN                     Off;
  EFI_SMBIOS_HANDLE         LocalHandle;

  TotalLen = HeaderLength;
  for (Index = 0; Index < StringCount; Index++) {
    TotalLen += AsciiStrSize (Strings[Index]);
  }

  TotalLen += 1;                              // final NUL when no strings
  if (StringCount == 0) {
    TotalLen += 1;                            // need two NULs in that case
  }

  Record = AllocateZeroPool (TotalLen);
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Record, Header, HeaderLength);
  Off = HeaderLength;
  for (Index = 0; Index < StringCount; Index++) {
    UINTN  Sz = AsciiStrSize (Strings[Index]);
    CopyMem ((UINT8 *)Record + Off, Strings[Index], Sz);
    Off += Sz;
  }

  // trailing NUL terminator(s) already zeroed by AllocateZeroPool.
  LocalHandle = SMBIOS_HANDLE_PI_RESERVED;
  Status      = Smbios->Add (Smbios, NULL, &LocalHandle, Record);
  if (Handle != NULL) {
    *Handle = LocalHandle;
  }

  FreePool (Record);
  return Status;
}

EFI_STATUS
EFIAPI
SunxiSmbiosEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_SMBIOS_PROTOCOL  *Smbios;
  EFI_SMBIOS_HANDLE    MemArrayHandle;
  EFI_SMBIOS_HANDLE    MemDeviceHandle;
  CHAR8                FirmwareVersionAscii[64];
  CHAR16              *FirmwareVersionUcs2;
  CHAR8                FirmwareVendorAscii[64];
  CHAR16              *FirmwareVendorUcs2;

  Status = gBS->LocateProtocol (
                  &gEfiSmbiosProtocolGuid,
                  NULL,
                  (VOID **)&Smbios
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiSmbiosDxe: SMBIOS protocol missing - %r\n", Status));
    return Status;
  }

  //
  // Convert the UCS-2 PCD strings to ASCII for Type 0/1.
  //
  FirmwareVendorUcs2  = (CHAR16 *)PcdGetPtr (PcdFirmwareVendor);
  FirmwareVersionUcs2 = (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString);
  if ((FirmwareVendorUcs2 == NULL) || (*FirmwareVendorUcs2 == 0)) {
    FirmwareVendorUcs2 = L"Orange Pi 4 Pro EDK2 Port";
  }

  if ((FirmwareVersionUcs2 == NULL) || (*FirmwareVersionUcs2 == 0)) {
    FirmwareVersionUcs2 = L"v0.2-EHCI";
  }

  UnicodeStrToAsciiStrS (FirmwareVendorUcs2,  FirmwareVendorAscii,  sizeof (FirmwareVendorAscii));
  UnicodeStrToAsciiStrS (FirmwareVersionUcs2, FirmwareVersionAscii, sizeof (FirmwareVersionAscii));

  //
  // Type 0 - BIOS Information
  //
  {
    SMBIOS_TABLE_TYPE0  Type0;
    CONST CHAR8        *Strs[3];

    ZeroMem (&Type0, sizeof (Type0));
    Type0.Hdr.Type     = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
    Type0.Hdr.Length   = sizeof (Type0);
    Type0.Vendor       = 1;
    Type0.BiosVersion  = 2;
    Type0.BiosReleaseDate = 3;
    Type0.BiosSize        = 0;                // 64KB units; 0 = 64KB image
    *(UINT64 *)&Type0.BiosCharacteristics = 0;
    Type0.BiosCharacteristics.BiosIsUpgradable      = 1;
    Type0.BiosCharacteristics.BootFromCdIsSupported = 1;
    Type0.BiosCharacteristics.SelectableBootIsSupported = 1;
    Type0.BIOSCharacteristicsExtensionBytes[0] = 0;
    Type0.BIOSCharacteristicsExtensionBytes[1] =
      BIT0 |   // ACPI supported
      BIT2 |   // UEFI Spec supported
      BIT3;    // SMBIOS table is virtual machine? (no) - leave 0
    Type0.SystemBiosMajorRelease         = 0;
    Type0.SystemBiosMinorRelease         = 2;
    Type0.EmbeddedControllerFirmwareMajorRelease = 0xFF;
    Type0.EmbeddedControllerFirmwareMinorRelease = 0xFF;

    Strs[0] = FirmwareVendorAscii;
    Strs[1] = FirmwareVersionAscii;
    Strs[2] = (CONST CHAR8 *)__DATE__;        // build date as release date

    AddRecord (Smbios, &Type0, sizeof (Type0), Strs, 3, NULL);
  }

  //
  // Type 1 - System Information
  //
  {
    SMBIOS_TABLE_TYPE1  Type1;
    CONST CHAR8        *Strs[5];

    ZeroMem (&Type1, sizeof (Type1));
    Type1.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
    Type1.Hdr.Length = sizeof (Type1);
    Type1.Manufacturer = 1;
    Type1.ProductName  = 2;
    Type1.Version      = 3;
    Type1.SerialNumber = 4;
    Type1.SKUNumber    = 5;
    Type1.Family       = 5;                   // reuse SKU string
    Type1.WakeUpType   = SystemWakeupTypePowerSwitch;

    Strs[0] = "Xunlong";
    Strs[1] = "Orange Pi 4 Pro";
    Strs[2] = "v1.0";
    Strs[3] = "0000000000000000";
    Strs[4] = "Allwinner A733";

    AddRecord (Smbios, &Type1, sizeof (Type1), Strs, 5, NULL);
  }

  //
  // Type 2 - Baseboard
  //
  {
    SMBIOS_TABLE_TYPE2  Type2;
    CONST CHAR8        *Strs[4];

    ZeroMem (&Type2, sizeof (Type2));
    Type2.Hdr.Type   = EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION;
    Type2.Hdr.Length = sizeof (Type2);
    Type2.Manufacturer = 1;
    Type2.ProductName  = 2;
    Type2.Version      = 3;
    Type2.SerialNumber = 4;
    Type2.FeatureFlag.Motherboard = 1;
    Type2.BoardType    = BaseBoardTypeMotherBoard;

    Strs[0] = "Xunlong";
    Strs[1] = "Orange Pi 4 Pro";
    Strs[2] = "v1.0";
    Strs[3] = "0000000000000000";

    AddRecord (Smbios, &Type2, sizeof (Type2), Strs, 4, NULL);
  }

  //
  // Type 3 - Chassis
  //
  {
    SMBIOS_TABLE_TYPE3  Type3;
    CONST CHAR8        *Strs[3];

    ZeroMem (&Type3, sizeof (Type3));
    Type3.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
    Type3.Hdr.Length = sizeof (Type3);
    Type3.Manufacturer = 1;
    Type3.Type         = MiscChassisTypeHandHeld;
    Type3.Version      = 2;
    Type3.SerialNumber = 3;
    Type3.BootupState      = ChassisStateSafe;
    Type3.PowerSupplyState = ChassisStateSafe;
    Type3.ThermalState     = ChassisStateSafe;
    Type3.SecurityStatus   = ChassisSecurityStatusNone;

    Strs[0] = "Xunlong";
    Strs[1] = "v1.0";
    Strs[2] = "0000000000000000";

    AddRecord (Smbios, &Type3, sizeof (Type3), Strs, 3, NULL);
  }

  //
  // Type 4 - Processor
  // The Allwinner A733 is an octa-core ARMv8.2-A: 4x Cortex-A76 + 4x
  // Cortex-A55 in a big.LITTLE arrangement. We report a single physical
  // socket with 8 cores / 8 threads. Speeds are nominal — A76 max
  // ~2.0 GHz, A55 ~1.8 GHz.
  //
  {
    SMBIOS_TABLE_TYPE4  Type4;
    CONST CHAR8        *Strs[5];

    ZeroMem (&Type4, sizeof (Type4));
    Type4.Hdr.Type   = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
    Type4.Hdr.Length = sizeof (Type4);
    Type4.Socket            = 1;
    Type4.ProcessorType     = CentralProcessor;
    Type4.ProcessorFamily   = ProcessorFamilyIndicatorFamily2;
    Type4.ProcessorManufacturer = 2;
    // PROCESSOR_ID_DATA layout differs per arch; leaving zeros is OK.
    Type4.ProcessorVersion  = 3;
    Type4.Voltage.ProcessorVoltageCapability3_3V = 0;
    Type4.Voltage.ProcessorVoltageCapability5V   = 0;
    Type4.ExternalClock     = 24;                 // sys24M reference
    Type4.MaxSpeed          = 2000;
    Type4.CurrentSpeed      = 1800;
    // Status: bit6 = socket populated, bits[2:0] = 001 (CPU enabled).
    Type4.Status            = 0x41;
    Type4.ProcessorUpgrade  = ProcessorUpgradeUnknown;
    Type4.SerialNumber      = 4;
    Type4.AssetTag          = 5;
    Type4.PartNumber        = 5;
    Type4.CoreCount         = 8;
    Type4.EnabledCoreCount  = 8;
    Type4.ThreadCount       = 8;
    Type4.ProcessorCharacteristics = (UINT16)(
        BIT2 |   // 64-bit capable
        BIT3 |   // multi-core
        BIT5 |   // execute protection
        BIT6     // enhanced virtualization
        );
    Type4.ProcessorFamily2  = ProcessorFamilyARMv8;

    Strs[0] = "CPU0";
    Strs[1] = "Allwinner";
    Strs[2] = "Allwinner A733 (4x A76 + 4x A55)";
    Strs[3] = "0000000000000000";
    Strs[4] = "A733";

    AddRecord (Smbios, &Type4, sizeof (Type4), Strs, 5, NULL);
  }

  //
  // Type 16 - Physical Memory Array (single 6 GB LPDDR4X bank)
  //
  {
    SMBIOS_TABLE_TYPE16  Type16;

    ZeroMem (&Type16, sizeof (Type16));
    Type16.Hdr.Type   = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
    Type16.Hdr.Length = sizeof (Type16);
    Type16.Location  = MemoryArrayLocationSystemBoard;
    Type16.Use       = MemoryArrayUseSystemMemory;
    Type16.MemoryErrorCorrection = MemoryErrorCorrectionNone;
    Type16.MaximumCapacity       = (UINT32)A733_MEMORY_SIZE_MB * 1024;     // KB
    Type16.MemoryErrorInformationHandle = 0xFFFE;
    Type16.NumberOfMemoryDevices = 1;

    AddRecord (Smbios, &Type16, sizeof (Type16), NULL, 0, &MemArrayHandle);
  }

  //
  // Type 17 - Memory Device (LPDDR4X soldered)
  //
  {
    SMBIOS_TABLE_TYPE17  Type17;
    CONST CHAR8         *Strs[5];

    ZeroMem (&Type17, sizeof (Type17));
    Type17.Hdr.Type   = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
    Type17.Hdr.Length = sizeof (Type17);
    Type17.MemoryArrayHandle = MemArrayHandle;
    Type17.MemoryErrorInformationHandle = 0xFFFE;
    Type17.TotalWidth = 32;
    Type17.DataWidth  = 32;
    Type17.Size       = (UINT16)A733_MEMORY_SIZE_MB;       // MB
    Type17.FormFactor = MemoryFormFactorChip;
    Type17.DeviceLocator = 1;
    Type17.BankLocator   = 2;
    Type17.MemoryType    = MemoryTypeLpddr4;
    Type17.Speed         = 3200;
    Type17.ConfiguredMemoryClockSpeed = 3200;
    Type17.Manufacturer  = 3;
    Type17.SerialNumber  = 4;
    Type17.AssetTag      = 5;
    Type17.PartNumber    = 5;

    Strs[0] = "DRAM0";
    Strs[1] = "BANK0";
    Strs[2] = "Unknown";
    Strs[3] = "0000000000000000";
    Strs[4] = "LPDDR4X-3200";

    AddRecord (Smbios, &Type17, sizeof (Type17), Strs, 5, &MemDeviceHandle);
  }

  //
  // Type 19 - Memory Array Mapped Address
  // PcdSystemMemoryBase = 0x40000000, Size = 0x180000000 (6 GB).
  //
  {
    SMBIOS_TABLE_TYPE19  Type19;
    UINT64               Base;
    UINT64               Size;

    Base = PcdGet64 (PcdSystemMemoryBase);
    Size = PcdGet64 (PcdSystemMemorySize);

    ZeroMem (&Type19, sizeof (Type19));
    Type19.Hdr.Type   = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
    Type19.Hdr.Length = sizeof (Type19);
    Type19.StartingAddress = 0xFFFFFFFF;        // use Extended fields
    Type19.EndingAddress   = 0xFFFFFFFF;
    Type19.MemoryArrayHandle = MemArrayHandle;
    Type19.PartitionWidth    = 1;
    Type19.ExtendedStartingAddress = Base;
    Type19.ExtendedEndingAddress   = Base + Size - 1;

    AddRecord (Smbios, &Type19, sizeof (Type19), NULL, 0, NULL);
  }

  //
  // Type 32 - System Boot Information (no errors detected)
  //
  {
    SMBIOS_TABLE_TYPE32  Type32;

    ZeroMem (&Type32, sizeof (Type32));
    Type32.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
    Type32.Hdr.Length = sizeof (Type32);
    Type32.BootStatus = BootInformationStatusNoError;

    AddRecord (Smbios, &Type32, sizeof (Type32), NULL, 0, NULL);
  }

  DEBUG ((DEBUG_INFO, "SunxiSmbiosDxe: published Type 0/1/2/3/4/16/17/19/32\n"));
  return EFI_SUCCESS;
}
