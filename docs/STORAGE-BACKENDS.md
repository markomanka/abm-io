# Storage backends: one contract, several places to put the bytes

A design note, not a plan. Nothing here is scheduled; see [ROADMAP.md](ROADMAP.md) for what is. Its purpose
is to record, before anyone writes a cloud path, what may be borrowed from the database world, what may not,
and how a second place to put the bytes can exist without the science depending on which one was used.

Related: [INTEGRATION.md](INTEGRATION.md) is the contract these backends must all honour, especially its
*Checkpoint files* section; [FINDINGS.md](FINDINGS.md) holds the measurement behind the one decision already
taken here; [RATIONALE.md](RATIONALE.md) entry 10 records that decision with its alternatives;
`bench/interrupted_checkpoint.py` is the harness, so the experiment can be repeated on another storage
system.

## Backends, not a fork

A cloud or object-storage path must be a **backend behind the same contract**, not a fork. A fork duplicates
the semantics, and the semantics are exactly what must not diverge: the same schema, the same strict resume,
the same restored state, whether the bytes went to a parallel file system or to an object store.

This generalises the invariant already written for compression in [INTEGRATION.md](INTEGRATION.md): the
storage layer is invisible to the science. Two checkpoints of the same simulation, one written to each
backend, restore identically. Like the compression invariant, that is testable, and provable in the bounded
sense, which is what makes a second backend safe to add later.

## What the database world has that we lack

**Snapshot visibility, and it is already adopted.** A busy cluster or a preemptible instance kills writers
mid-write. The data-lake table formats solved this with a manifest written last: a checkpoint becomes
visible only when every part exists. Measured here: over a hundred kills, the naive layout left a reader
with a usable checkpoint 15 times out of 100 and four times with a file that was listed and could not be
opened, while the manifest layout gave a complete checkpoint 100 times out of 100
([FINDINGS.md](FINDINGS.md)). This is the single idea in this family that transfers to the write path
intact, and it is now part of the contract.

**Retry-safe naming, for object storage.** A write retried after a 5xx must not half-overwrite its
predecessor. Content-addressed or attempt-numbered part names make a retry harmless; a fixed name does not.
Not needed on a POSIX file system, needed on day one of a cloud path.

**Object-store semantics, rather than POSIX pretended.** Rename is atomic within a POSIX directory and is
not in most object stores. The contract therefore states the *property*, that a checkpoint becomes visible
only when complete, and leaves the mechanism to the backend: a conditional write or a versioned pointer
achieves there what rename achieves here.

## What the database world does not have for us

**Transactions.** Their machinery, write-ahead logging, locking, multi-version visibility, exists to resolve
conflicts between concurrent transactions. A checkpoint has no conflicts: each rank owns its agents, nobody
updates another's, and the pattern is bulk append, written once and usually read once. Against that
workload a DBMS adds write amplification, row orientation where we want columns, and a server competing for
the cores and memory the simulation is using. Most HPC centres will not run one on compute nodes in any
case.

**Quorum writes, and this is the trap worth naming.** Quorums make a distributed database tolerant of a slow
or dead node because the data is replicated. A checkpoint needs *every* rank's agents, so there is no
redundancy to exploit and no quorum to reach: one straggler stalls the checkpoint whatever the storage does.
The answer to that comes from the HPC side instead, in the form of multilevel checkpointing: ranks write
fast to node-local storage, parity is encoded across nodes so that losing one node is recoverable, and the
result drains to the parallel file system in the background. Established libraries implement this pattern;
their current state should be checked before relying on any of it.

## Where the database people are simply right

Once checkpoints exist, people will want to ask questions across them: how a population changed over a run,
how two runs differ, what a parameter sweep produced. Writing a query engine for that would be absurd when
Parquet, read by DuckDB, Polars, pandas or R, answers it. That is an **export format beside the checkpoint
format**, not a replacement: checkpoints restore a simulation exactly, exports answer questions
conveniently. The column layout the checkpoints already use makes the export nearly free.

## The experiments

Three, of which one is done:

1. **Formats.** Write a million agents' state as per-rank HDF5, as Parquet, and as a chunked array on object
   storage, measuring throughput, peak memory and time to restore. Not done. It decides whether Parquet is
   an export or could be the checkpoint format itself.
2. **Interruption.** Kill a writer mid-checkpoint, with and without a manifest, and see what a reader
   observes. **Done**: see [FINDINGS.md](FINDINGS.md) and `bench/interrupted_checkpoint.py`. It decided the
   manifest.
3. **Object storage under faults.** Repeat the second against an S3-compatible store with injected latency
   and 5xx errors. Not done, and it needs a store rather than new code: the harness exists. It decides what
   replaces rename, and whether retry-safe naming is sufficient.

## What this note does not propose

No database, no new dependency, and no schedule. A cloud backend is worth building when someone has a cloud
run to do; until then this note exists so that the decisions taken now, the manifest above all, do not
quietly assume a POSIX file system.
