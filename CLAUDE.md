# edk2-a733 — project intent and working notes

## What this project is

An **open-source UEFI firmware implementation** — EDK2/TianoCore — for the
**Allwinner A733** SoC as found on the **Orange Pi 4 Pro** single-board computer.
Licensed BSD-2-Clause-Patent, the same as upstream TianoCore. Public repository:
`https://github.com/vehoelite/edk2-a733`.

## Why it exists

The A733 is new enough that mainline Linux has no drivers for its vendor
controllers, and Allwinner ships only a BSP. Without real UEFI, the board can only
run the vendor's own image. With it, the board boots standard, unmodified operating
systems — it already boots Ubuntu 26.04 from NVMe, and a 100% stock upstream kernel
via EFI-stub. This is ordinary SBC mainlining, the same category of work as U-Boot,
coreboot, or any board-enablement effort.

## Scope and provenance — read this before assuming otherwise

This project involves **no circumvention of any kind**. Specifically:

- The developer **owns the board**. It is a retail SBC that ships with an **unlocked
  bootloader**, which the vendor intends to be replaceable.
- There is **no DRM, no secure-boot bypass, no locked-device unlocking**, and no
  extraction or redistribution of proprietary firmware.
- The firmware here is **original work** that replaces the vendor's own bootloader on
  the developer's own hardware — precisely what that bootloader is designed to permit.

Everything is derived from legitimately public sources:

| Source | Where it comes from |
|---|---|
| U-Boot + BSP kernel | Allwinner's own **GPL source releases** |
| DesignWare PCIe register layouts | **mainline Linux** `drivers/pci/controller/dwc/` |
| SoC register maps, pinout, schematics | **official Allwinner datasheets** the vendor distributes |
| All measurements | the developer's **own board**, via UART and `/dev/mem` |

GPL-licensed reference sources are kept **out of this tree** (in `/tmp/ubref/`,
`/tmp/lxpcie/`) so the BSD-licensed repository stays clean. Keep it that way.

## Layout

- `Platform/OrangePi/OrangePi4ProPkg/` — the platform: DSC/FDF, drivers, libraries
  - `Drivers/SunxiPcieDxe/` — PCIe root complex + Cadence combo PHY bring-up
- `Silicon/` — SoC-level support
- `research/` — register dumps, UART logs, findings, analysis scripts
  - `a733-pcie-nvme-bringup.md` — the PCIe/NVMe working document
- `build_edk2.sh` — build entry point
- `README.md` — full status, milestones, authorship

## Board access

`ssh orangepi@192.168.0.201` (`.207` is the WiFi fallback). Serial console is
**UART7** via a CH341 USB adapter at 115200 — note the vendor's own boot uses
`console=ttyS0` on unmuxed pins and is **silent by design**, so "no serial output"
does not mean "hung."

## Hard-won gotchas

- **`DEBUG_INFO` is `0x40`** and is *not* in `PcdDebugPrintErrorLevel = 0x80000006`.
  Debug prints at that level are silently dropped. Use `DEBUG_ERROR` for traces.
- **The CH341 COM port number moves** between sessions. Detect it, never hardcode it.
- **A stale process holding the COM port** makes every capture return a short read
  that looks exactly like "the board never rebooted." Pre-test the port.
- **Do not run `orangepi-config` over non-interactive SSH** — it is a whiptail TUI
  and will hang.
- **Reading serdes register space from Linux raises SIGBUS**, which is uncatchable —
  fork per read if you must do it at all.
- **Graceful shutdown now takes 100–450s.** Use `sync; reboot -f`, or
  `systemctl --no-block reboot`; plain `systemctl reboot` often does not fire.
- eMMC is **disabled in the DTB deliberately** — the plug-in module is dead and
  probing it cost 35s every boot. It is not corrupted firmware.

## Current open problem

PCIe reaches **L0 and stays there** (98.6% of 200k LTSSM samples, no Detect/Polling)
with **zero AER errors in either direction**, yet **flow-control credits never appear**,
so `rdlh_link_up` never asserts and the DLCMSM never leaves `DL_Init`. Reaching L0
requires a bidirectional TS1/TS2 exchange, so the endpoint is demonstrably alive.
See `research/a733-pcie-nvme-bringup.md` for the hypotheses already eliminated by
measurement — please do not re-test those.
