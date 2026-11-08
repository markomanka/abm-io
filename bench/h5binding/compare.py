#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Runs the binding comparison of docs/RATIONALE.md entry 12 and reports it.

    bench/h5binding/compare.py BUILD_DIR [--agents N] [--chunk C] [--cache BYTES]

Runs BUILD_DIR/abmio_h5binding_compare in a fresh directory, then compares the
files the bindings wrote: `h5diff` for contents, `h5dump -pH` for structure
and storage layout, `cmp` for bytes, and h5py with hdf5plugin as the
third-party reader the contract promises. Tools that are missing are reported
as missing, never silently skipped. Timings from a shared machine are
indicative; the decision uses the dedicated benchmark machine.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def tool(name):
    path = shutil.which(name)
    if path is None:
        print(f"{name}: not installed")
    return path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("build_dir")
    p.add_argument("--agents", type=int, default=1000000)
    p.add_argument("--chunk", type=int, default=16384)
    p.add_argument("--cache", type=int, default=0)
    p.add_argument("--keep", action="store_true", help="keep the files")
    args = p.parse_args()
    binary = os.path.join(args.build_dir, "abmio_h5binding_compare")
    if not os.path.exists(binary):
        sys.exit(f"{binary} not found: build with -DABMIO_HDF5=ON")

    work = tempfile.mkdtemp(prefix="abmio-h5binding-")
    print(f"files in {work}")
    r = run([binary, "--agents", str(args.agents), "--chunk", str(args.chunk),
             "--cache", str(args.cache), "--dir", work])
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    failed = r.returncode != 0

    files = sorted(f for f in os.listdir(work) if f.endswith(".h5"))
    by_variant = {}
    for f in files:
        name, variant = f[:-3].rsplit("-", 1)
        by_variant.setdefault(variant, []).append((name, os.path.join(work, f)))

    h5diff, h5dump, cmp = tool("h5diff"), tool("h5dump"), tool("cmp")
    # The HDF5 command-line tools decode the LZ4 filter only through the
    # plugin, which hdf5plugin ships; without it h5diff exits 2 on these files.
    try:
        import hdf5plugin
        os.environ["HDF5_PLUGIN_PATH"] = hdf5plugin.PLUGIN_PATH
        print(f"HDF5_PLUGIN_PATH={hdf5plugin.PLUGIN_PATH} (from hdf5plugin)")
    except ImportError:
        print("hdf5plugin not installed: the HDF5 tools cannot read the LZ4 files")
    for variant, pairs in sorted(by_variant.items()):
        if len(pairs) < 2:
            print(f"{variant}: one binding only, nothing to compare")
            continue
        (a_name, a), (b_name, b) = pairs[0], pairs[1]
        print(f"\n== {variant}: {a_name} vs {b_name}")
        if h5diff:
            # -c reports differences in object properties too (filters, chunking).
            d = run([h5diff, "-c", a, b])
            print("h5diff -c (contents and properties):",
                  "identical" if d.returncode == 0 and not d.stdout.strip()
                  else "cannot compare (exit 2: a filter is missing?)"
                  if d.returncode == 2 else f"differ\n{d.stdout}{d.stderr}")
        if h5dump:
            # The first line names the file; everything after it is the structure.
            da = run([h5dump, "-pH", a]).stdout.split("\n", 1)[1]
            db = run([h5dump, "-pH", b]).stdout.split("\n", 1)[1]
            same = da == db
            print("h5dump -pH (structure and layout):",
                  "identical" if same else "differ")
            if not same:
                import difflib
                for line in difflib.unified_diff(da.splitlines(), db.splitlines(),
                                                 a_name, b_name, lineterm="", n=1):
                    print("   ", line)
        if cmp:
            c = run([cmp, "-l", a, b])
            differing = len(c.stdout.splitlines())
            print("cmp (bytes):", "identical" if c.returncode == 0 else
                  f"{differing} bytes differ of {os.path.getsize(a)} and "
                  f"{os.path.getsize(b)}; first at offset "
                  f"{c.stdout.split()[0] if c.stdout.split() else '?'}")

    # Compression is semantically invisible: the compressed and the plain
    # file of the same binding hold the same contents.
    if h5diff and "raw" in by_variant and "lz4" in by_variant:
        for (name, raw), (_, lz4) in zip(by_variant["raw"], by_variant["lz4"]):
            d = run([h5diff, raw, lz4])
            print(f"\n== {name}: raw vs lz4, h5diff (contents):",
                  "identical" if d.returncode == 0 and not d.stdout.strip()
                  else f"differ or cannot compare (exit {d.returncode})")

    print("\n== third-party reader: h5py")
    try:
        import h5py  # noqa: F401
        import hdf5plugin  # noqa: F401
        import numpy as np
    except ImportError as e:
        print(f"not available: {e}")
    else:
        for f in files:
            path = os.path.join(work, f)
            with h5py.File(path, "r") as h:
                pos = h["/agents/cell/position"]
                uid = h["/agents/cell/uid"][:]
                n = len(uid)
                ok = (uid == np.arange(1, n + 1)).all()
                last = pos[n - 1]
                i = n - 1
                expect = ((i % 1000) * 0.5, ((i // 1000) % 1000) * 0.5,
                          (i // 1000000) * 0.5)
                ok = ok and tuple(last) == expect
                comps = [s.decode() if isinstance(s, bytes) else s
                         for s in h["/components/name"][:]]
                affects = h["/components/affects_results"][:]
                ok = ok and comps == ["biodynamo", "libroadrunner", "paraview"]
                ok = ok and affects.dtype == bool and list(affects) == [True, True, False]
                filters = pos.compression, dict(pos._filters) if hasattr(pos, "_filters") else {}
                print(f"{f}: {'reads correctly' if ok else 'WRONG'}; {n} agents;"
                      f" filters {filters[1]}; chunks {pos.chunks}")
                failed = failed or not ok

    if not args.keep:
        shutil.rmtree(work)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
