#!/bin/bash
# Run as root on BSP. Copies the vendor kernel modules + firmware into the NVMe Ubuntu rootfs.
set -e
R=/mnt/nvme
mount /dev/nvme0n1p1 $R 2>/dev/null || true
echo "=== vendor modules on BSP ==="
ls -d /lib/modules/5.15.147-sun60iw2 2>&1
du -sh /lib/modules/5.15.147-sun60iw2 2>/dev/null

echo "=== copy modules tree into Ubuntu rootfs ==="
mkdir -p $R/lib/modules
rsync -a /lib/modules/5.15.147-sun60iw2 $R/lib/modules/ 2>&1 | tail -2
echo "copied modules: $(ls -d $R/lib/modules/5.15.147-sun60iw2 2>&1)"

echo "=== copy firmware (wifi/bt blobs etc.) ==="
mkdir -p $R/lib/firmware
rsync -a /lib/firmware/ $R/lib/firmware/ 2>&1 | tail -1
echo "firmware copied"

echo "=== run depmod against the Ubuntu rootfs so modules resolve ==="
for d in dev proc sys; do mount --bind /$d $R/$d 2>/dev/null || true; done
chroot $R depmod -a 5.15.147-sun60iw2 2>&1 | tail -2
echo "depmod done"

echo "=== verify key network modules present ==="
ls $R/lib/modules/5.15.147-sun60iw2/kernel/drivers/net/ethernet/ 2>/dev/null | head
find $R/lib/modules/5.15.147-sun60iw2 -iname "*sunxi*mac*" -o -iname "*dwmac*" -o -iname "*aw_*" 2>/dev/null | head

sync
for d in sys proc dev; do umount $R/$d 2>/dev/null || true; done
umount $R 2>/dev/null || true
rm -f /boot/skip_edk2; touch /boot/try_edk2; sync
echo "armed: $(ls /boot/try_edk2)"
echo DONE
