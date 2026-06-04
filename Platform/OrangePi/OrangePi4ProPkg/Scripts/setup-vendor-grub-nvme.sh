#!/bin/bash
# runs as root on the board via: sudo su -c
set -e
mount /dev/mmcblk1p2 /mnt/esp 2>/dev/null || true
NVUUID=$(blkid -s UUID -o value /dev/nvme0n1p1)
echo "NVMe root UUID = $NVUUID"

cat > /mnt/esp/boot/grub/grub.cfg <<EOF
set timeout=5
set default=0

menuentry "Ubuntu 26.04 (vendor kernel, NVMe root)" {
	echo "loading vendor DTB..."
	devicetree /a733-vendor.dtb
	echo "loading vendor kernel..."
	linux /vmlinuz-vendor root=UUID=$NVUUID rootwait rootdelay=8 rootfstype=ext4 console=ttyS0,115200 earlycon=sunxi-uart,0x02500000 loglevel=7
	echo "loading vendor initrd..."
	initrd /initrd-vendor
	echo "booting vendor kernel -> NVMe Ubuntu..."
}
menuentry "UEFI Firmware Settings" { fwsetup }
EOF

echo "=== grub.cfg ==="
cat /mnt/esp/boot/grub/grub.cfg
echo "=== ESP payload check ==="
ls -l /mnt/esp/vmlinuz-vendor /mnt/esp/a733-vendor.dtb /mnt/esp/initrd-vendor /mnt/esp/EFI/boot/bootaa64.efi 2>&1
sync
umount /mnt/esp 2>/dev/null || true
# arm EDK2
rm -f /boot/skip_edk2; touch /boot/try_edk2; sync
echo "armed: $(ls /boot/try_edk2 2>&1)"
