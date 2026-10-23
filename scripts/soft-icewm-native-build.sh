#!/bin/bash
# Native (non-docker) clean build of the soft-float X11 + IceWM Korvo-1 image.
# Equivalent to rebuild-soft-x11-de.sh + image-full-build.sh, for the Ubuntu
# x64 server: all buildroot host deps are installed system-wide, the dl cache
# lives in $REPO/dl, and the output dir follows the repo Makefile convention
# ($REPO/build/korvo1).  Carries the 2026-10-23 hotpatch fixes in-tree:
#   - no /etc/wpa_supplicant.conf in the rootfs (Wi-Fi auth-storm kill switch;
#     put wpa_supplicant.conf on SD p1 to re-enable Wi-Fi, see S40wifi)
#   - /root/mw.sh memory sampler, s31.ppm wallpaper, desktop-mannered
#     jpegdec/jpegenc, fputest-abi, icewm prefs (all via local-overlay)
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$REPO/build/korvo1   # buildroot output; artifacts land in build/artifacts
DL=$REPO/dl
JOBS=$(nproc)

cd "$REPO/external/buildroot"
MAKE="make O=$OUT BR2_EXTERNAL=$REPO/br2-external BR2_DL_DIR=$DL BR2_JLEVEL=$JOBS"

echo "[$(date +%T)] clean + defconfig (jobs=$JOBS)"
$MAKE clean
$MAKE korvo1_defconfig

echo "[$(date +%T)] config checks"
grep -qx 'BR2_GCC_TARGET_ABI="ilp32"' $OUT/.config
grep -qx '# BR2_RISCV_ISA_RVF is not set' $OUT/.config
grep -qx 'BR2_PACKAGE_XSERVER_XORG_SERVER=y' $OUT/.config
grep -qx 'BR2_PACKAGE_MATCHBOX=y' $OUT/.config
grep -qx 'BR2_PACKAGE_ICEWM=y' $OUT/.config

echo "[$(date +%T)] full build start"
$MAKE

echo "[$(date +%T)] target checks"
READELF=$(ls $OUT/host/bin/*-linux-musl-readelf | head -1)
"$READELF" -h $OUT/target/bin/busybox | grep -q 'Flags:.*0x1, RVC, soft-float ABI'
test -x $OUT/target/usr/bin/Xorg
test -x $OUT/target/usr/bin/xterm
test -x $OUT/target/usr/bin/icewm || test -x $OUT/target/usr/bin/icewm-session
test -x $OUT/target/usr/bin/matchbox-panel
test -x $OUT/target/usr/bin/startde
test -x $OUT/target/usr/bin/jpegdec
test -x $OUT/target/usr/bin/jpegenc
test -x $OUT/target/usr/bin/fputest-abi
test -f $OUT/target/usr/share/backgrounds/s31.ppm
test -f $OUT/target/root/mw.sh
test ! -e $OUT/target/etc/wpa_supplicant.conf

echo "[$(date +%T)] export artifacts"
mkdir -p $REPO/build/artifacts
cp $OUT/images/Image $REPO/build/artifacts/Image-soft
cp $OUT/images/linux.size $REPO/build/artifacts/linux-soft.size
cp $OUT/images/sdcard.img $REPO/build/artifacts/sdcard-soft-icewm.img

ls -lh $REPO/build/artifacts/
sha256sum $REPO/build/artifacts/Image-soft \
          $REPO/build/artifacts/sdcard-soft-icewm.img
echo "[$(date +%T)] NATIVE_BUILD_DONE"
