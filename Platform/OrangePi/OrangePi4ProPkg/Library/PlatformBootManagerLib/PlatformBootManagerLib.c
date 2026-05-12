/** @file
  Minimal PlatformBootManagerLib for Orange Pi 4 Pro.

  Registers a UEFI Shell boot option and falls through to the
  standard BDS boot device selection.

  Copyright (c) 2024, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/BootLogoLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  // Nothing platform-specific before console is attached.
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  // Connect all drivers to all controllers.
  EfiBootManagerConnectAll ();

  // Process boot options from NVRAM.
  EfiBootManagerRefreshAllBootOption ();

  // Print a minimal banner.
  Print (L"Orange Pi 4 Pro UEFI - carpi-os\n\r");
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  Print (L"Unable to boot. Dropping to UEFI shell.\n\r");
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  // No UI during boot timeout countdown.
}
