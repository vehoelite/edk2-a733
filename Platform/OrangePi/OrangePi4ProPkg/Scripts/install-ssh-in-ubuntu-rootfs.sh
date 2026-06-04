#!/bin/bash
set -e
R=/mnt/nvme
mountpoint -q $R || mount /dev/nvme0n1p1 $R
for d in dev dev/pts proc sys run; do mount --bind /$d $R/$d 2>/dev/null || true; done

echo "=== give chroot REAL dns ==="
rm -f $R/etc/resolv.conf
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > $R/etc/resolv.conf

echo "=== replace cdrom apt sources with real Ubuntu archive (26.04 resolute, deb822) ==="
# disable any cdrom source
find $R/etc/apt -name '*.list' -o -name '*.sources' 2>/dev/null | xargs -r grep -l cdrom 2>/dev/null | while read f; do mv "$f" "$f.disabled"; done
cat > $R/etc/apt/sources.list.d/ubuntu.sources <<EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: resolute resolute-updates resolute-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

echo "=== dns test ==="
chroot $R /bin/bash -c "getent hosts ports.ubuntu.com >/dev/null && echo DNS_OK || echo DNS_FAIL"

echo "=== apt update + install openssh-server ==="
chroot $R /bin/bash -c "export DEBIAN_FRONTEND=noninteractive; apt-get update -qq 2>&1 | tail -3; apt-get install -y openssh-server 2>&1 | tail -6"

echo "=== enable ssh + root login ==="
chroot $R systemctl enable ssh 2>&1 | tail -1
mkdir -p $R/etc/ssh/sshd_config.d
echo "PermitRootLogin yes" > $R/etc/ssh/sshd_config.d/99-root.conf
chroot $R systemctl enable NetworkManager 2>&1 | tail -1

echo "=== verify ==="
echo "sshd binary: $(ls $R/usr/sbin/sshd 2>&1)"
echo "ssh enabled: $(ls $R/etc/systemd/system/multi-user.target.wants/ 2>/dev/null | grep -i ssh || echo NO)"
echo "NM enabled:  $(ls $R/etc/systemd/system/multi-user.target.wants/ 2>/dev/null | grep -i NetworkManager || echo NO)"

sync
for d in run sys proc dev/pts dev; do umount $R/$d 2>/dev/null || true; done
umount $R 2>/dev/null || true
echo DONE
