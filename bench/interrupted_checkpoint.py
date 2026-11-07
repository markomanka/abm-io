#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Does a manifest written last make a half-finished checkpoint invisible?

A checkpoint is a directory of per-rank HDF5 files plus a pointer to the newest
complete one. A writer is killed at a random moment while writing; a reader
then opens whatever it can find. Two writers are compared:

  in-place:    the pointer names the checkpoint before it exists and each rank
               writes straight to its final name, the way a directory of files
               is usually produced;
  manifest:    each rank writes to a temporary name and renames it, then the
               manifest listing every rank is written to a temporary name and
               renamed, and only then is the pointer replaced.

Reported: what the reader sees, over many trials. The results are in
`docs/FINDINGS.md`; the rule they led to is in `docs/INTEGRATION.md`.

Usage: bench/interrupted_checkpoint.py {in-place|manifest} TRIALS

Needs h5py and numpy, which abm-io itself does not: this is a measurement
harness, not part of the library.
"""
import json, os, random, signal, sys, time
import h5py
import numpy as np

RANKS = 16
AGENTS_PER_RANK = 150000
FIELDS = ("position_x", "position_y", "position_z", "diameter", "volume")

def write_rank(path, rank, step):
    with h5py.File(path, "w") as f:
        g = f.create_group("agents/Cell")
        for i, name in enumerate(FIELDS):
            data = np.full(AGENTS_PER_RANK, step * 1000 + rank * 10 + i, dtype="f8")
            g.create_dataset(name, data=data, chunks=(4096,))
        f.attrs["rank"] = rank
        f.attrs["step"] = step

def write_checkpoint(root, step, mode):
    d = os.path.join(root, f"step_{step}")
    os.makedirs(d, exist_ok=True)
    names = []
    for rank in range(RANKS):
        final = os.path.join(d, f"rank_{rank}.h5")
        if mode == "in-place":
            if rank == 0:                                 # the pointer moves first
                with open(os.path.join(root, "latest"), "w") as p:
                    p.write(json.dumps({"step": step}))
            write_rank(final, rank, step)                 # visible while growing
        else:
            tmp = final + ".tmp"
            write_rank(tmp, rank, step)
            os.replace(tmp, final)                        # each file appears whole
        names.append(os.path.basename(final))
    if mode == "manifest":
        man, tmp = os.path.join(d, "manifest.json"), os.path.join(d, "manifest.json.tmp")
        with open(tmp, "w") as m:
            json.dump({"step": step, "ranks": RANKS, "files": names}, m)
            m.flush(); os.fsync(m.fileno())
        os.replace(tmp, man)
        ptmp = os.path.join(root, "latest.tmp")
        with open(ptmp, "w") as p:
            json.dump({"step": step}, p); p.flush(); os.fsync(p.fileno())
        os.replace(ptmp, os.path.join(root, "latest"))    # the checkpoint appears here

def read_checkpoint(root, mode):
    """What a reader sees: 'complete', 'partial', 'corrupt' or 'none'."""
    pointer = os.path.join(root, "latest")
    if not os.path.exists(pointer):
        return "none"
    try:
        with open(pointer) as p:
            state = json.load(p)
    except Exception:
        return "corrupt"
    d = os.path.join(root, f"step_{state['step']}")
    if mode == "manifest":
        try:
            with open(os.path.join(d, "manifest.json")) as m:
                listed = json.load(m)["files"]
        except Exception:
            return "partial"            # the pointer names a checkpoint with no manifest
    else:
        listed = [f"rank_{r}.h5" for r in range(RANKS)]   # every rank is expected
        missing = [n for n in listed if not os.path.exists(os.path.join(d, n))]
        if missing:
            return "partial"            # the pointer names what is not all there
    seen = 0
    for name in listed:
        path = os.path.join(d, name)
        try:
            with h5py.File(path, "r") as f:
                g = f["agents/Cell"]
                if any(len(g[field]) != AGENTS_PER_RANK for field in FIELDS):
                    return "corrupt"
            seen += 1
        except Exception:
            return "corrupt"            # listed but unreadable: the worst outcome
    return "complete" if seen == RANKS else "partial"

def main():
    mode, trials = sys.argv[1], int(sys.argv[2])
    root = f"/tmp/manifest/store_{mode}"
    os.system(f"rm -rf {root}; mkdir -p {root}")
    write_checkpoint(root, 1, mode)     # one good checkpoint to fall back on
    counts = {}
    for trial in range(trials):
        pid = os.fork()
        if pid == 0:
            write_checkpoint(root, 2 + trial, mode)
            os._exit(0)
        time.sleep(random.uniform(0.005, 0.115))   # kill somewhere in the middle
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
        seen = read_checkpoint(root, mode)
        counts[seen] = counts.get(seen, 0) + 1
        for name in os.listdir(root):               # back to the baseline
            if name.startswith("step_") and name != "step_1":
                os.system(f"rm -rf {os.path.join(root, name)}")
        with open(os.path.join(root, "latest"), "w") as p:
            json.dump({"step": 1}, p)
    print(f"  {mode:12} over {trials} kills: " +
          ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))

if __name__ == "__main__":
    main()
