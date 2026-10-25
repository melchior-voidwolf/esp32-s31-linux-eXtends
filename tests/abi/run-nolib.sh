#!/bin/sh
# Run seven alternating rounds by default; binaries are freestanding/static.
set -eu
ROUNDS=${1:-7}
DIR=${2:-/tmp/nolib}
case $ROUNDS in ''|*[!0-9]*) echo "usage: $0 [rounds] [binary-dir]" >&2; exit 2 ;; esac

round=1
while [ "$round" -le "$ROUNDS" ]; do
    if [ $((round % 2)) = 1 ]; then
        order="soft-base hard-base soft-full hard-full hard-full soft-full hard-base soft-base"
    else
        order="hard-full soft-full hard-base soft-base soft-base hard-base soft-full hard-full"
    fi
    for variant in $order; do
        echo "VARIANT=$variant ROUND=$round"
        "$DIR/nolib-$variant"
    done
    round=$((round + 1))
done
echo "NOLIB_BOARD_DONE rounds=$ROUNDS"
