#!/bin/bash
# Build and QEMU-verify diagnostics later downloaded to /tmp on the board.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:-korvo1flashf}
HOST=$ROOT/build/$BOARD/host/bin
OUT=$ROOT/build/$BOARD/board-diagnostics
TC=
for candidate in "$HOST"/*-linux-musl-gcc; do
    if [ -x "$candidate" ]; then TC=${candidate%gcc}; fi
done
[ -n "$TC" ] || { echo "no cross toolchain in $HOST" >&2; exit 1; }
mkdir -p "$OUT"
case $BOARD in
    korvo1flashf) ABI=ilp32f ;;
    korvo1flash) ABI=ilp32 ;;
    *) echo "unsupported BOARD=$BOARD" >&2; exit 2 ;;
esac
MARCH=rv32imafc_zba_zbb_zbs
CPU=rv32,f=true,d=false,c=true,zba=true,zbb=true,zbs=true

"${TC}gcc" -O1 -static -march="$MARCH" -mabi="$ABI" \
    -o "$OUT/fputest" "$ROOT/../../ppa-work/fputest/fputest.c"
"${TC}gcc" -O2 -static -march="$MARCH" -mabi="$ABI" \
    -o "$OUT/fputest-abi" "$ROOT/../../ppa-work/fputest/fputest-abi.c"
RUNTIME_FLAGS=
[ "$ABI" = ilp32f ] || RUNTIME_FLAGS=-DSOFT_FLOAT_ABI
"${TC}gcc" -O2 -march="$MARCH" -mabi="$ABI" $RUNTIME_FLAGS \
    -o "$OUT/abi-runtime-smoke" "$ROOT/tests/abi/abi-runtime-smoke.c" -lm

qemu-riscv32 -cpu "$CPU" "$OUT/fputest"
qemu-riscv32 -cpu "$CPU" "$OUT/fputest-abi"
qemu-riscv32 -cpu "$CPU" -L "$ROOT/build/$BOARD/target" "$OUT/abi-runtime-smoke"
echo "BOARD_DIAGNOSTICS_BUILD_OK $OUT"
