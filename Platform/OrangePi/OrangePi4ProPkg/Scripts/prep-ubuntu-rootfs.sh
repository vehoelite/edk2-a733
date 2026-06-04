#!/bin/bash
SP='sshpass -p orangepi ssh -o StrictHostKeyChecking=no -o ConnectTimeout=15 orangepi@192.168.0.207'
$SP 'R=/mnt/nvme; \
  echo "=== 1. fstab ==="; \
  NVUUID=$(sudo -S <<<orangepi blkid -s UUID -o value /dev/nvme0n1p1); \
  printf "UUID=%s / ext4 defaults,noatime 0 1\n" "$NVUUID" | sudo -S <<<orangepi tee $R/etc/fstab; \
  echo "=== 2. root pw=orangepi ==="; \
  sudo -S <<<orangepi chroot $R /bin/bash -c "echo root:orangepi | chpasswd" 2>&1; \
  echo "=== 3. serial getty ttyS0 ==="; \
  sudo -S <<<orangepi chroot $R systemctl enable serial-getty@ttyS0.service 2>&1 | tail -1; \
  echo "=== 4. de-casper ==="; \
  sudo -S <<<orangepi chroot $R /bin/bash -c "systemctl disable casper 2>/dev/null; rm -f /etc/systemd/system/*casper* /etc/init.d/casper* 2>/dev/null; true"; \
  echo "=== 5. hostname + user orangepi/orangepi ==="; \
  echo opi-ubuntu | sudo -S <<<orangepi tee $R/etc/hostname >/dev/null; \
  sudo -S <<<orangepi chroot $R /bin/bash -c "id orangepi 2>/dev/null || (useradd -m -s /bin/bash orangepi && echo orangepi:orangepi | chpasswd && usermod -aG sudo orangepi)" 2>&1 | tail -1; \
  echo "=== 6. PERMISSIONS HARDENING (your catch: too-lax perms block services/ssh) ==="; \
  sudo -S <<<orangepi chmod 0755 $R $R/etc $R/usr $R/var 2>/dev/null; \
  sudo -S <<<orangepi chmod 0700 $R/root 2>/dev/null; \
  sudo -S <<<orangepi chmod 0755 $R/etc/ssh 2>/dev/null; \
  sudo -S <<<orangepi chmod 0644 $R/etc/passwd $R/etc/group 2>/dev/null; \
  sudo -S <<<orangepi chmod 0640 $R/etc/shadow 2>/dev/null; \
  sudo -S <<<orangepi chmod 1777 $R/tmp 2>/dev/null; \
  sudo -S <<<orangepi chown -R root:root $R/etc $R/usr $R/bin $R/sbin 2>/dev/null; \
  echo "perms hardened"; \
  sync; sudo -S <<<orangepi umount /mnt/nvme 2>&1 && echo "NVMe unmounted clean"'
