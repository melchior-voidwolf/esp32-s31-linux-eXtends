# Buildroot ABI/ISA tests

- `abi-runtime-smoke.c` covers dynamic float calls, musl `sqrtf`, double-without-D, `setjmp/longjmp` F-register restore, signal, fork/wait and `clock_gettime`. The ilp32f build checks fs0 restoration explicitly. The soft build verifies only setjmp control flow because musl soft-float does not guarantee saving F registers across setjmp/longjmp; it must not be described as offering the same inline-F state guarantees.
- `isa-abi-*` is the same freestanding benchmark source used for the OpenWrt controlled tests, rebuilt here with one Buildroot GCC for all four `ilp32/ilp32f × base/full-Zb` variants.
- `run-nolib.sh` alternates AB/BA order over seven rounds by default. Analyze the raw log with `isa-abi-analyze.py`; use per-group minima as well as medians and do not present microbenchmark speedups as whole-system gains.

Host commands after building `korvo1flashf`:

```sh
python3 -I scripts/check-ilp32f.py --board korvo1flashf
bash scripts/build-abi-nolib.sh korvo1flashf
```

Benchmark binaries are written to `build/korvo1flashf/abi-nolib/`, not into the flash rootfs. They have no libc, libgcc or dynamic-loader dependency. No audio operation is part of these tests.
