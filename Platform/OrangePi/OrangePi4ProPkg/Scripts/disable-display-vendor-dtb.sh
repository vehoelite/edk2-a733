#!/bin/bash
set -e
mount /dev/mmcblk1p2 /mnt/esp 2>/dev/null || true
echo "=== decompile vendor DTB, find display nodes ==="
dtc -I dtb -O dts /mnt/esp/a733-vendor.dtb > /tmp/vd.dts 2>/dev/null
echo "display-ish nodes present:"
grep -nE "sunxi-drm|de@|tcon|dsi@|edp@|hdmi@|hdmi_codec|disp@|display-engine" /tmp/vd.dts | head -20
echo "=== disable each via fdtput (status=disabled) ==="
for node in \
  /soc@3000000/sunxi-drm \
  /soc@3000000/de@5000000 \
  /soc@3000000/tcon0@5501000 /soc@3000000/tcon1@5502000 /soc@3000000/tcon2@5503000 \
  /soc@3000000/tcon3@5730000 /soc@3000000/tcon4@5731000 \
  /soc@3000000/dsi0@5506000 /soc@3000000/dsi1@5508000 \
  /soc@3000000/edp0@5720000 /soc@3000000/hdmi0@5520000 /soc@3000000/hdmi_codec ; do
  fdtput -t s /mnt/esp/a733-vendor.dtb "$node" status disabled 2>/dev/null && echo "  disabled $node" || echo "  (no $node)"
done
echo "=== verify key ones ==="
echo "sunxi-drm: $(fdtget /mnt/esp/a733-vendor.dtb /soc@3000000/sunxi-drm status 2>&1)"
echo "de:        $(fdtget /mnt/esp/a733-vendor.dtb /soc@3000000/de@5000000 status 2>&1)"
echo "=== sanity: nvme/pcie/mmc NOT touched ==="
echo "pcie/serdes still present? $(fdtget /mnt/esp/a733-vendor.dtb /soc@3000000/serdes@6c00000 compatible 2>&1 | head -c40)"
sync
umount /mnt/esp 2>/dev/null || true
rm -f /boot/skip_edk2; touch /boot/try_edk2; sync
echo "armed: $(ls /boot/try_edk2 2>&1)"
