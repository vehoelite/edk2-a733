/** @file
  SunxiUsbDxe - publish A733 USB host controllers as non-discoverable
  devices so the EDKII XhciDxe / EhciDxe drivers attach to them.

  Confirmed bases (from Linux /proc/iomem on this exact board):
     0x04101000  EHCI0  (size 0x400)
     0x04200000  EHCI1  (size 0x400)
     0x06A00000  xHCI2  (size 0x8000)  USB 3.0

  BSP U-Boot has already programmed CCU clocks, USB PHY and VBUS
  regulators before handing off to BL33, so we don't initialise any
  of that here. If the board exits BSP U-Boot with USB powered down,
  this driver will not bring it back up - we'd need clock-gate code.

  Copyright (c) 2026, carpi-os contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

EFI_STATUS
EFIAPI
SunxiUsbDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  // xHCI2 (USB3) at 0x06A00000 is NOT brought up by BSP U-Boot - its
  // CapLength register reads 0 at our entry, causing XhciDxe to ASSERT.
  // Linux brings xHCI up later via CCU clocks + USB PHY init we don't
  // yet replicate. Skip it for now and stick to the EHCI controllers
  // which the BSP does power on for its SPL USB stack.
#if 0
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             0x06A00000ULL,
             0x00008000ULL
             );
  DEBUG ((DEBUG_INFO, "SunxiUsbDxe: xHCI @ 0x06A00000 register: %r\n", Status));
#endif

  // EHCI0 - USB 2.0
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             0x04101000ULL,
             0x00000400ULL
             );
  DEBUG ((DEBUG_INFO, "SunxiUsbDxe: EHCI0 @ 0x04101000 register: %r\n", Status));

  // EHCI1 - USB 2.0
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             0x04200000ULL,
             0x00000400ULL
             );
  DEBUG ((DEBUG_INFO, "SunxiUsbDxe: EHCI1 @ 0x04200000 register: %r\n", Status));

  return EFI_SUCCESS;
}
