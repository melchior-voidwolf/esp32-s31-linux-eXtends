#!/bin/bash
# Apply the audio patch to a build tree, compile the codec object, then restore.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=$ROOT/build/korvo1flashf/build/linux-7.3
PATCH=$ROOT/linux/patches/0018-es8389-tdm-clock-and-binding.patch
BACKUP=$(mktemp -d /tmp/es8389-patch-test.XXXXXX)
restore() {
    cp "$BACKUP/es8389.c" "$KDIR/sound/soc/codecs/es8389.c"
    cp "$BACKUP/everest,es8389.yaml" "$KDIR/Documentation/devicetree/bindings/sound/everest,es8389.yaml"
    rm -f "$KDIR/sound/soc/codecs/es8389.o"
    rm -rf "$BACKUP"
}
trap restore EXIT
cp "$KDIR/sound/soc/codecs/es8389.c" "$BACKUP/es8389.c"
cp "$KDIR/Documentation/devicetree/bindings/sound/everest,es8389.yaml" "$BACKUP/everest,es8389.yaml"
patch -d "$KDIR" -p1 -F0 -s < "$PATCH"
make -C "$KDIR" ARCH=riscv \
    CROSS_COMPILE="$ROOT/build/korvo1flashf/host/bin/riscv32-buildroot-linux-musl-" \
    -j32 sound/soc/codecs/es8389.o
printf 'ES8389_PATCH_COMPILE_PASS\n'
