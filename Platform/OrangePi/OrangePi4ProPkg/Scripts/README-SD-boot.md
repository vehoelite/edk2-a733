# Booting Debian from the SD card under EDK2 (no USB stick)

As of 2026-06-03 the A733 port boots Debian **entirely from the microSD card** —
EDK2 reads the kernel/initrd/dtb off a FAT32 ESP on the SD, with no USB mass
storage attached. This retires the USB-stick dependency.

## The boot chain

```
U-Boot (try_edk2 flag) ──▶ EDK2 (our BL33)
   └─ SunxiMmcDxe        installs EFI_BLOCK_IO for SMHC0 @0x04020000 (the SD)
   └─ PartitionDxe       parses the MBR, finds partition 2 (the FAT ESP)
   └─ Fat.inf            mounts FAT32 ──▶ EFI_SIMPLE_FILE_SYSTEM
   └─ BootDebian         finds \Image on the first volume that has it,
                         installs \board.dtb as the FDT config table,
                         registers \initrd via LINUX_EFI_INITRD_MEDIA,
                         LoadImage/StartImage with the Debian cmdline
   └─ EFI-stub kernel ──▶ Debian (headless: UART + ssh)
```

No firmware code change was needed for the SD-boot milestone — `SunxiMmcDxe`,
`PartitionDxe`, `Fat.inf` and `BootDebian` were already compiled in. The only
missing piece was a **populated FAT partition on the SD**, created as below.

## SD layout

The 512 GB boot SD (`/dev/mmcblk1`, msdos label) is partitioned:

| Part | Start (s)   | Sectors    | Type            | Use                         |
|------|-------------|------------|-----------------|-----------------------------|
| p1   | 65536       | 990150656  | 0x83 Linux ext4 | Debian rootfs (UNTOUCHED)   |
| p2   | 990216192   | 10027008   | 0x0c FAT32 LBA  | EDK2 ESP (~4.78 GB tail)    |

p2 lives entirely in the previously-unallocated tail, so creating it does not
touch the rootfs.

## Recreate the ESP (run on the board)

```sh
# 1. create the FAT partition in the free tail (does NOT touch p1)
sudo parted -s /dev/mmcblk1 unit s mkpart primary fat32 990216192s 1000243199s
sudo partx -a --nr 2 /dev/mmcblk1            # expose /dev/mmcblk1p2 (p1 stays mounted)
sudo mkfs.vfat -F 32 -n EDK2ESP /dev/mmcblk1p2

# 2. regenerate the headless board.dtb (see disable_display.py)
sudo cp /sys/firmware/fdt /tmp/live.dtb
dtc -I dtb -O dts -o /tmp/live.dts /tmp/live.dtb
python3 disable_display.py /tmp/live.dts /tmp/board.dts
dtc -I dts -O dtb -o /tmp/board.dtb /tmp/board.dts     # ~215 KB

# 3. populate the ESP
sudo mkdir -p /mnt/esp && sudo mount /dev/mmcblk1p2 /mnt/esp
sudo cp /home/orangepi/touchfix/build/ksrc/arch/arm64/boot/Image /mnt/esp/Image
sudo cp /boot/initrd.img-5.15.147-sun60iw2               /mnt/esp/initrd
sudo cp /tmp/board.dtb                                    /mnt/esp/board.dtb
sync && sudo umount /mnt/esp
```

BootDebian looks for exactly `\Image`, `\initrd`, `\board.dtb` at the FAT root.

## Verifying a successful SD boot

After `try_edk2` is armed and the board is power-cycled, a successful native
EDK2-from-SD boot shows, in the booted Debian:

- `/sys/firmware/efi` **present** (booted under UEFI),
- `/proc/cmdline` ends in `panic=10` with **no** `BOOT_IMAGE=` and **no**
  `earlyprintk=sunxi-uart` (that is the BootDebian cmdline, not the BSP one),
- `/dev/mmcblk1p2` present, **no `/dev/sd*`** (USB pulled).
