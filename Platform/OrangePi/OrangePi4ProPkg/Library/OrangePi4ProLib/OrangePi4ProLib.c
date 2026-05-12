/** @file
  Minimal ArmPlatformLib for Orange Pi 4 Pro (A733).

  Provides the board-specific callbacks required by ArmPlatformPkg/PrePi.
  Most values come from PCDs; this file only handles the few that must be
  coded in C (MPIDR parsing, memory descriptor table).

  Copyright (c) 2024, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>

#include <A733.h>

// ---------------------------------------------------------------------------
// Core presence
// A733 DynamIQ FLAT topology (confirmed from live DTB, cpu@N nodes):
//   Each core is the sole member of its own Aff1 group (DynamIQ style).
//   cpu@0   → A55, MPIDR Aff1=0, Aff0=0 → position 0
//   cpu@100 → A55, MPIDR Aff1=1, Aff0=0 → position 1
//   cpu@200 → A55, MPIDR Aff1=2, Aff0=0 → position 2
//   cpu@300 → A55, MPIDR Aff1=3, Aff0=0 → position 3
//   cpu@400 → A55, MPIDR Aff1=4, Aff0=0 → position 4
//   cpu@500 → A55, MPIDR Aff1=5, Aff0=0 → position 5
//   cpu@600 → A76, MPIDR Aff1=6, Aff0=0 → position 6
//   cpu@700 → A76, MPIDR Aff1=7, Aff0=0 → position 7
// Aff0 is always 0.  Position == Aff1.
// Boot core: A55 core 0 (MPIDR = 0x00000000)
// ---------------------------------------------------------------------------
UINTN
ArmPlatformGetCorePosition (
  IN UINTN MpId
  )
{
  // In the flat DynamIQ layout the position is simply the Aff1 field.
  return (MpId >> 8) & 0xFF;
}

UINTN
ArmPlatformGetPrimaryCoreMpId (VOID)
{
  // Boot on A55 core 0 (Aff1=0, Aff0=0) — confirmed from DTS topology
  return 0x00000000;
}

UINTN
ArmPlatformIsPrimaryCore (
  IN UINTN MpId
  )
{
  return (UINTN)((MpId & 0xFFFFFF) == ArmPlatformGetPrimaryCoreMpId ());
}

// ---------------------------------------------------------------------------
// Called from PrePi SEC before memory is initialized.
// TF-A / U-Boot SPL already initialized DRAM; nothing to do here.
// ---------------------------------------------------------------------------
RETURN_STATUS
ArmPlatformInitialize (
  IN UINTN MpId
  )
{
  return RETURN_SUCCESS;
}

// ---------------------------------------------------------------------------
// Virtual memory map passed to ArmConfigureMmu() in SEC.
// ---------------------------------------------------------------------------
ARM_MEMORY_REGION_DESCRIPTOR  gVirtualMemoryTable[] = {
  // DRAM (cached, normal memory) — 6 GB on Orange Pi 4 Pro
  {
    .PhysicalBase = A733_DRAM_BASE,
    .VirtualBase  = A733_DRAM_BASE,
    .Length       = 0x180000000UL,                // 6 GB (matches PcdSystemMemorySize)
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
  },
  // Device registers (UART, GIC, CCU, PIO …)
  {
    .PhysicalBase = 0x00000000UL,
    .VirtualBase  = 0x00000000UL,
    .Length       = 0x40000000UL,                 // 1 GB device space
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE,
  },
  // End of table
  { 0, 0, 0, 0 },
};

VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  *VirtualMemoryMap = gVirtualMemoryTable;
}
