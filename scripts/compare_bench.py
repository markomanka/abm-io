#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare benchmark results with a stored baseline, or record a new baseline.

Times (keys ending in _ns) fail when slower than the baseline by more than --tolerance.
Sizes (bytes_per_agent) fail when they grow by more than 1%.
Usage: compare_bench.py BASELINE CURRENT [--tolerance 0.05] [--record]
"""
import argparse
import json
import sys


def results(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)["results"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("baseline")
    ap.add_argument("current")
    ap.add_argument("--tolerance", type=float, default=0.05)
    ap.add_argument("--record", action="store_true", help="write CURRENT to BASELINE instead of comparing")
    args = ap.parse_args()
    if args.record:
        with open(args.current, encoding="utf-8") as src, open(args.baseline, "w", encoding="utf-8") as dst:
            json.dump(json.load(src), dst, indent=2)
            dst.write("\n")
        print(f"recorded baseline {args.baseline}")
        return 0
    try:
        base = results(args.baseline)
    except FileNotFoundError:
        print(f"::warning::no baseline at {args.baseline}; run the Benchmarks workflow with record=true first")
        return 0
    cur = results(args.current)
    regressions = []
    print("| metric | baseline | current | ratio | status |")
    print("| --- | --- | --- | --- | --- |")
    for key in sorted(base):
        if key not in cur:
            continue
        b, c = base[key], cur[key]
        ratio = c / b if b else 1.0
        limit = 1 + (args.tolerance if key.endswith("_ns") else 0.01)
        ok = ratio <= limit
        if not ok:
            regressions.append(key)
        print(f"| {key} | {b:.2f} | {c:.2f} | {ratio:.3f} | {'ok' if ok else 'REGRESSION'} |")
    if regressions:
        print(f"::error::{len(regressions)} regression(s): {', '.join(regressions)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
