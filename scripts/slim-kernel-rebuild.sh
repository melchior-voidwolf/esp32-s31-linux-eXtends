#!/bin/bash
# Incremental rebuild after a linux.config change: re-extract/patch/build the
# kernel, re-run post-image (new IMAGE.BIN inside boot.vfat), export.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$REPO/build/korvo1
DL=$REPO/dl
JOBS=$(nproc)

cd "$REPO/external/buildroot"
MAKE="make O=$OUT BR2_EXTERNAL=$REPO/br2-external BR2_DL_DIR=$DL BR2_JLEVEL=$JOBS"

echo "[$(date +%T)] linux-dirclean + rebuild"
$MAKE linux-dirclean
$MAKE

echo "[$(date +%T)] checks"
test -f $OUT/images/Image
grep -c 'CONFIG_NET_VENDOR' $OUT/build/linux-7.3/.config >/dev/null
N=$(grep -cE '^CONFIG_NET_VENDOR.*=y' $OUT/build/linux-7.3/.config || true)
echo "NET_VENDOR=y remaining: $N"
test "$N" = 0

echo "[$(date +%T)] export artifacts"
mkdir -p $REPO/build/artifacts
cp $OUT/images/Image $REPO/build/artifacts/Image-soft
cp $OUT/images/linux.size $REPO/build/artifacts/linux-soft.size
cp $OUT/images/sdcard.img $REPO/build/artifacts/sdcard-soft-icewm.img

ls -lh $REPO/build/artifacts/
sha256sum $REPO/build/artifacts/Image-soft $REPO/build/artifacts/sdcard-soft-icewm.img
echo "[$(date +%T)] SLIM_BUILD_DONE"
