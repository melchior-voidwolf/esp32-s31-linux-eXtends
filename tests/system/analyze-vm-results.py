#!/usr/bin/env python3
"""Summarize runtime vm.min_free_kbytes A/B logs without hiding slow samples."""

import argparse
import collections
import json
from pathlib import Path
import re
import statistics

RESULT = re.compile(
    r"RESULT mode=(\S+) count=(\d+) ns=(\d+) ns_per_op=[0-9.]+ mtd_read_sectors=(\d+)"
)
EXPECTED_COUNTS = {
    "getpid": 1000000,
    "clock_gettime": 1000000,
    "fork_exec_true": 100,
    "squashfs_read": 30,
}


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    samples = collections.defaultdict(list)
    completeness = {}
    for path in sorted(args.directory.glob("r*-v*.log")):
        text = path.read_text(errors="replace")
        errors = []
        name_match = re.fullmatch(r"r(\d+)-v(\d+)\.log", path.name)
        if not name_match:
            errors.append("filename does not match rN-vM.log")
            file_round = file_value = None
        else:
            file_round, file_value = map(int, name_match.groups())
        markers = re.findall(r"VM_ACTUAL=(\d+) VARIANT=(\d+) ROUND=(\d+)", text)
        actual = variant = round_number = None
        if len(markers) != 1:
            errors.append(f"expected exactly one VM marker, got {len(markers)}")
        else:
            actual, variant, round_number = map(int, markers[0])
            if not name_match or (actual, variant, round_number) != (file_value, file_value, file_round):
                errors.append("filename/actual/variant/round mismatch")

        matches = list(RESULT.finditer(text))
        modes = [match.group(1) for match in matches]
        if len(matches) != len(EXPECTED_COUNTS) or set(modes) != set(EXPECTED_COUNTS):
            errors.append(f"expected one result per required mode, got {modes}")
        elif len(set(modes)) != len(modes):
            errors.append("duplicate result mode")
        for match in matches:
            mode, count, _, _ = match.groups()
            if int(count) != EXPECTED_COUNTS[mode]:
                errors.append(f"wrong count for {mode}: {count}")
        if "SYSTEM_BENCH_DONE" not in text:
            errors.append("missing SYSTEM_BENCH_DONE")

        completeness[path.name] = {
            "actual": actual,
            "variant": variant,
            "round": round_number,
            "complete": not errors,
            "errors": errors,
        }
        sample_value = str(actual) if actual is not None else "unknown"
        for match in matches:
            mode, count, ns, sectors = match.groups()
            samples[(sample_value, mode)].append({
                "ns": int(ns), "count": int(count), "sectors": int(sectors),
                "file": path.name,
            })

    summary = {"complete": completeness, "modes": {}}
    modes = sorted({mode for _, mode in samples})
    for mode in modes:
        print(f"\n[{mode}]")
        summary["modes"][mode] = {}
        for value in ("512", "768", "1024", "unknown"):
            rows = samples.get((value, mode), [])
            if not rows:
                continue
            elapsed = [row["ns"] for row in rows]
            sectors = [row["sectors"] for row in rows]
            summary["modes"][mode][value] = {
                "n": len(rows),
                "median_ns": statistics.median(elapsed),
                "min_ns": min(elapsed),
                "p95_ns": percentile(elapsed, .95),
                "max_ns": max(elapsed),
                "median_mtd_sectors": statistics.median(sectors),
                "max_mtd_sectors": max(sectors),
                "samples": rows,
            }
            print(f"min_free={value:>5} n={len(rows)} "
                  f"median={statistics.median(elapsed)/1e9:.6f}s "
                  f"p95={percentile(elapsed,.95)/1e9:.6f}s "
                  f"max={max(elapsed)/1e9:.6f}s "
                  f"mtd_sectors(median/max)={statistics.median(sectors)}/{max(sectors)}")

    bad = {name: row for name, row in completeness.items() if not row["complete"]}
    run_info = (args.directory / "run-info.txt").read_text()
    rounds_match = re.search(r"^rounds=(\d+)", run_info, re.M)
    rounds = int(rounds_match.group(1)) if rounds_match else 0
    if rounds <= 0:
        bad["run-info.txt"] = "rounds must be positive"
    expected = {f"r{round_number}-v{value}.log"
                for round_number in range(1, rounds + 1)
                for value in (512, 768, 1024)}
    missing = sorted(expected - set(completeness))
    if missing:
        bad["missing"] = missing
    summary["all_complete"] = not bad
    output = args.directory / "summary.json"
    output.write_text(json.dumps(summary, indent=2) + "\n")
    if bad:
        print(f"VM_RESULTS_INCOMPLETE {bad}")
        raise SystemExit(1)
    print(f"VM_RESULTS_COMPLETE json={output}")


if __name__ == "__main__":
    main()
