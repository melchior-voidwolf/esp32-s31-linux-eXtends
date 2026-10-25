#!/usr/bin/env python3
"""Synthetic tests for analyze-vm-results.py; no board or audio access."""

import json
from pathlib import Path
import subprocess
import tempfile

ANALYZER = Path(__file__).resolve().with_name("analyze-vm-results.py")
MODES = (("getpid", 1000000), ("clock_gettime", 1000000),
         ("fork_exec_true", 100), ("squashfs_read", 30))


def populate(directory):
    (directory / "run-info.txt").write_text("original_min_free_kbytes=1024\nrounds=3\n")
    for round_number in (1, 2, 3):
        for value in (512, 768, 1024):
            lines = [f"VM_ACTUAL={value} VARIANT={value} ROUND={round_number}"]
            for mode, count in MODES:
                elapsed = 100000000 + value * 1000 + round_number
                lines.append(
                    f"RESULT mode={mode} count={count} ns={elapsed} "
                    f"ns_per_op=1.0 mtd_read_sectors={value // 16}"
                )
            lines.append("SYSTEM_BENCH_DONE mem_available_kb=4096")
            (directory / f"r{round_number}-v{value}.log").write_text("\n".join(lines))


def run(directory):
    return subprocess.run(["python3", "-I", str(ANALYZER), str(directory)],
                          capture_output=True, text=True, check=False)


def main():
    with tempfile.TemporaryDirectory(prefix="s31-vm-analyzer-") as tmp:
        directory = Path(tmp)
        populate(directory)
        result = run(directory)
        assert result.returncode == 0, (result.stdout, result.stderr)
        summary = json.loads((directory / "summary.json").read_text())
        assert summary["all_complete"] is True
        assert summary["modes"]["getpid"]["512"]["n"] == 3
        (directory / "r2-v768.log").write_text("VM_ACTUAL=768\nincomplete\n")
        result = run(directory)
        assert result.returncode == 1, (result.stdout, result.stderr)

        populate(directory)
        path = directory / "r1-v512.log"
        path.write_text(path.read_text().replace("VM_ACTUAL=512", "VM_ACTUAL=768"))
        assert run(directory).returncode == 1

        populate(directory)
        path.write_text(path.read_text() + "\nRESULT mode=getpid count=1000000 ns=1 ns_per_op=1.0 mtd_read_sectors=0\n")
        assert run(directory).returncode == 1

        populate(directory)
        (directory / "run-info.txt").write_text("original_min_free_kbytes=1024\nrounds=0\n")
        assert run(directory).returncode == 1

        populate(directory)
        (directory / "r3-v1024.log").unlink()
        assert run(directory).returncode == 1
    print("VM_ANALYZER_TEST_PASS")


if __name__ == "__main__":
    main()
