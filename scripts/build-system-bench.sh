#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:-korvo1flashf}
HOST=$ROOT/build/$BOARD/host/bin
OUT=$ROOT/build/$BOARD/system-bench
CC=
for candidate in "$HOST"/*-linux-musl-gcc; do
    if [ -x "$candidate" ]; then CC=$candidate; fi
done
[ -n "$CC" ] || { echo "no cross gcc in $HOST" >&2; exit 1; }
mkdir -p "$OUT"
case $BOARD in
    korvo1flashf) ABI=ilp32f ;;
    korvo1flash) ABI=ilp32 ;;
    *) echo "unsupported BOARD=$BOARD" >&2; exit 2 ;;
esac
"$CC" -O2 -mabi="$ABI" -march=rv32imafc_zba_zbb_zbs \
    -o "$OUT/s31-system-bench" "$ROOT/tests/system/s31-system-bench.c"
if command -v qemu-riscv32 >/dev/null 2>&1; then
    qemu-riscv32 -cpu rv32,f=true,d=false,c=true,zba=true,zbb=true,zbs=true \
        -L "$ROOT/build/$BOARD/target" "$OUT/s31-system-bench" quick
fi
echo "SYSTEM_BENCH_BUILD_OK $OUT/s31-system-bench"
