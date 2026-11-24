#!/usr/bin/env bash
# Build an initramfs; all output stays in OUTPUT. No host clock or package changes.
set -euo pipefail
if [ "$#" != 6 ]; then
  echo 'usage: prepare-clock-vm.sh OUTPUT KERNEL MODULES_ROOT KERNEL_VERSION STATIC_BUSYBOX DROPBEAR_PREFIX' >&2
  exit 2
fi
output=$(realpath -m "$1")
kernel=$(realpath "$2")
modules=$(realpath "$3")
version=$4
busybox=$(realpath "$5")
dropbear=$(realpath "$6")
mkdir -p "$output/initramfs"; image="$output/initramfs"
cp "$kernel" "$output/vmlinuz"
kernel="$output/vmlinuz"
mkdir -p "$image"/{bin,proc,sys,dev,host,modules}
cp "$busybox" "$image/bin/busybox"
[ -f "$output/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -f "$output/id_ed25519"
cp "$output/id_ed25519.pub" "$image/authorized_keys"
# Preserve dependency order from the matching kernel's module metadata.
if [ ! -r "$modules/lib/modules/$version/modules.dep" ]; then
  depmod -b "$modules" "$version"
fi
for module in 9p 9pnet_virtio; do
  modprobe -d "$modules" -S "$version" --show-depends "$module"
done | awk '$1=="insmod" && !seen[$2]++ {print $2}' > "$output/modules.txt"
: > "$image/load-modules"
while read -r module; do
  name=$(basename "${module%.zst}")
  if [[ "$module" == *.zst ]]; then zstd -dc "$module" > "$image/modules/$name"; else cp "$module" "$image/modules/$name"; fi
  echo "/bin/busybox insmod /modules/$name" >> "$image/load-modules"
done < "$output/modules.txt"
cat > "$image/init" <<INIT
#!/bin/busybox sh
set -ex
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sys /sys
/bin/busybox mount -t devtmpfs dev /dev
. /load-modules
/bin/busybox mount -t 9p -o trans=virtio,version=9p2000.L,cache=loose,ro host /host
/bin/busybox mount -t proc proc /host/proc
/bin/busybox mount -t sysfs sys /host/sys
/bin/busybox mount -t devtmpfs dev /host/dev
/bin/busybox mount -t tmpfs tmp /host/tmp
/bin/busybox mount -t tmpfs run /host/run
/bin/busybox mount -t tmpfs root /host/root
/bin/busybox chmod 700 /host/root
/bin/busybox mkdir -p /host/root/.ssh
/bin/busybox cp /authorized_keys /host/root/.ssh/authorized_keys
/bin/busybox chmod 700 /host/root/.ssh
/bin/busybox chmod 600 /host/root/.ssh/authorized_keys
/bin/busybox ip link set eth0 up
/bin/busybox ip addr add 10.0.2.15/24 dev eth0
/bin/busybox ip route add default via 10.0.2.2
export LD_LIBRARY_PATH='$dropbear/usr/lib/x86_64-linux-gnu'
/bin/busybox chroot /host '$dropbear/usr/bin/dropbearkey' -t ed25519 -f /tmp/dropbear-key
exec /bin/busybox chroot /host '$dropbear/usr/sbin/dropbear' -F -E -s -g -r /tmp/dropbear-key -p 22
INIT
chmod +x "$image/init"
(cd "$image" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1) > "$output/initrd.gz"
node - "$output" "$kernel" <<'JS'
const fs = require("node:fs");
const [output, kernel] = process.argv.slice(2);
fs.writeFileSync(`${output}/config.json`, JSON.stringify({kernel, initrd: `${output}/initrd.gz`, privateKey: `${output}/id_ed25519`}, null, 2));
JS
