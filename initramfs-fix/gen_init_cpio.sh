#!/bin/bash
# Pack the initramfs cpio using the kernel's gen_init_cpio tool (no real
# device nodes needed — nodes are described textually).
set -e
VOL=esp32s31-br
IMG=esp32s31-buildroot:bookworm
REPO=/Users/kail/selfwork/esp32-S31/linux/repo
UID_=$(id -u); GID_=$(id -g)

cat > /tmp/initramfs.list <<'EOF'
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
file /init /work/initramfs-fix/root/init 0755 0 0
EOF

docker run --rm --user 0:0 -v "$VOL":/br -v "$REPO":/work -v /tmp/initramfs.list:/tmp/initramfs.list \
  "$IMG" sh -c '
    set -e
    # gen_init_cpio.c is a standalone host tool in the kernel source
    SRCDIR=$(dirname "$(find /br/korvo1/build -name gen_init_cpio.c | head -1)")
    gcc -O2 -o /tmp/gen_init_cpio "$SRCDIR/gen_init_cpio.c" -Wall
    /tmp/gen_init_cpio /tmp/initramfs.list | gzip -9 \
      > /work/initramfs-fix/initramfs.cpio.gz
    ls -la /work/initramfs-fix/initramfs.cpio.gz
    chown '"$UID_:$GID_"' /work/initramfs-fix/initramfs.cpio.gz
  '
