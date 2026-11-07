# Why the design is as it is

Decisions that were questioned, the alternatives, and what would change our mind.

## Why this document exists

A design is a set of choices, most of which look arbitrary from outside and some of which are. Code records
what was chosen; it records nothing about what was rejected, or why, or what would make the choice wrong.
That missing half is what makes a project hard to join, hard to criticise usefully, and hard to correct
years later when the reason has been forgotten and only the habit remains.

So each entry below says what was decided, what was rejected, why, and **what would change our mind**. The
last of these matters most. A rationale that only justifies is a defence; one that names the evidence that
would overturn it is a claim, and a claim can be tested. Where we have no evidence, the entry says so
rather than implying that a preference was a measurement.

This document exists because the project was criticised and the criticism was useful. Some of it was right
and changed the design; some rested on misreadings, and a text that is misread twice has told us where it
is unclear, which is worth as much. Nothing here is softened to make the project look better: an entry
that records a mistake is doing its job.

Criticism of this kind is welcome, from anyone, by any route, with or without a patch. The only thing asked
in return is specificity, because "this is fragile" cannot be acted on and "this breaks when a header uses
a template" can.

---

## 1. The schema is read from C++, not the C++ written from the schema

**Decided.** `abm-gen` parses a simulator's headers and emits `TypeTraits` specialisations.

**The alternative**, and the critique's main recommendation: define the schema in a language-neutral format
and generate the C++ structs from it. That guarantees schema and code agree, removes libclang, and makes
bindings for other languages easy.

**Why not.** BioDynaMo already exists, with tens of thousands of lines of agent and behaviour classes and
an unknown number of user models built on them. The contract's central promise is that user models need no
changes. Generating their classes would mean rewriting them, and rewriting the users' too. The inversion is
the right design for a simulator written against abm-io from the start, and nothing prevents it later: the
schema is already a data structure, so a generator in the other direction is additive.

**What the critique feared, and why it cannot happen.** "Silent mismatches between the compiled struct and
the extracted schema." `abm-gen` emits only names and pointers to members; every shape, element type and
count is derived by the compiler through `ShapeOf`. The parser's opinion about types is never used, so a
parser that misunderstands a type produces a compile error, not a wrong schema.

**The real residual risk** is different: a member or a class *missed entirely*, which no compiler catches.
Class templates are refused outright for this reason. Validation on BioDynaMo's own classes exists to
measure that risk.

**What would change our mind.** A new simulator written against abm-io, where generation is simply better;
or validation showing systematic misses that no reasonable parser can avoid.

## 2. Units are recorded, never converted

**Decided.** Files keep exactly what the simulator computed; the unit travels as metadata and is checked
for consistency when loading.

**The objection.** This helps archiving and hurts interoperability: if one simulator writes micrometres and
another expects millimetres, the friction lands on the user.

**Why the rule stands for checkpoints and exchange.** A conversion is a numerical act: it changes the
bytes, loses exactness, and makes a restart no longer bit-identical. Conversion is a semantic decision that
belongs where semantics live, in the translators of the model description, not in the I/O layer.

**What is worth taking.** The suggestion of canonical units for *export* is reasonable and not in conflict:
checkpoints stay exact, visualisation and analysis files could normalise. Recorded as an open option, not
decided, because nobody has yet been bitten.

**What would change our mind.** Evidence that readers mishandle units in practice despite the metadata.

## 3. Schema evolution: silent defaults → strict by default. **Changed.**

**Was decided.** A field missing from a file takes its default; an unknown field is skipped. Old files stay
readable after a model changes.

**The objection, which is right.** Resuming a checkpoint written before a parameter existed, and silently
giving that parameter a default, can produce scientifically wrong results with no warning. For analysis of
old files the permissive rule is right; for resuming a simulation it is dangerous.

**Now decided.** The two cases are separated. Reading for analysis stays permissive. **Resuming a
checkpoint is strict**: if the schema differs in anything that affects mechanics, the load fails and says
what differs, unless the caller explicitly accepts the defaults.

This is the clearest hit in the critique, and the cheapest to fix now rather than after someone loses a
month to it.

## 4. One file per process, with parallel HDF5 optional

**Decided.** Per-process checkpoint files plus an index are always supported; a single shared file through
parallel HDF5 is optional, never required.

**The objection.** On Lustre or GPFS, 100,000 ranks writing 100,000 files is a metadata bottleneck: closing
the files can take longer than the step that produced them.

**True, and not what was decided.** The decision was *never to require parallel HDF5*, not to write one
file per rank at any scale. The scaling path was named in the design reflection and should have been in the
decision itself: group writers, one file per node or per thousand ranks, or HDF5 subfiling, or ADIOS2.
Cloud object storage pulls the other way, favouring many independent objects over one shared file, which is
part of why the per-process form is the one that must always work.

**Alternatives considered.** Zarr and TileDB are serious options, particularly for object storage. Not
chosen now because HDF5 is what this community already reads without new tools, and because another
dependency needs a reason. Worth revisiting when cloud runs are real rather than planned.

**What would change our mind.** A measurement on a real parallel file system at a few thousand ranks. We
have none, and should not pretend otherwise.

## 5. Arenas and variable-length data

**The objection.** Zero-allocation arenas suit fixed-size fields, not the variable-length data biology
needs: neighbour lists, organelles, lineages. Forcing them into one arena will produce fragile memory code.

**Already the design.** Fixed-size fields travel as elements, in rows or columns; vectors, strings, records
and polymorphic lists are skipped by that path and travel separately, with per-type counts and offsets in
the message header. The separation the critique recommends is what the exchange does.

**That a careful reader concluded otherwise is a fault in our documents,** not in theirs. The contract
describes arenas and zero steady-state allocation without saying plainly that variable-length data takes a
different path.

## 6. The Rust experiment

**Decided.** A bounded experiment: the decoders in Rust behind a C interface, off by default, with a
decision at the end. The C++ decoders ship throughout.

**The objection.** Maintaining two codecs is a drain, and divergence would break exact restart. Decide now:
all Rust, or all C++.

**Why "decide now" was declined.** Deciding without evidence is how a project acquires a dependency it
cannot justify, or forgoes a guarantee it could have had cheaply. The evidence now exists: six proofs,
including memory safety of the LZ4 block decoder over every four-byte block and the round trip on all three
paths; no divergence from the C++ decoder over 216,000 inputs; and a measured cost of four attempts to find
bounds a solver can carry. The divergence fear cannot materialise while only one decoder ships, which is
what differential fuzzing is there to keep true.

**What will decide it.** The benchmark, on the dedicated machine. If the proofs come cheaply and the speed
matches, the codec core moves; otherwise `rust/` is deleted and C++ keeps it with CBMC and fuzzing.

## 7. Delta encoding and discrete events. **Idea adopted as an option.**

**Decided so far.** Levels 1 and 2: skip unchanged columns, and encode changed columns against their
previous version with XOR, shuffle and LZ4. An agent arriving on a rank has no base there and travels as a
full record.

**The objection, which is a good idea.** Biology is full of discrete topological events: division, death,
a new adhesion. Delta-encoding continuous variables is easy; delta-encoding topology changes is not, and
can produce payloads larger than sending the data raw. Use a dual path: deltas for continuous state, a
compact event stream for discrete events.

**Taken as an option for level 3,** where a change bitmap was the sketch. An event stream is likely better
where the events are sparse, and worse where they are dense; which is which is a measurement, not an
argument. Recorded so the choice is made on numbers.

## 8. Checkpoints compress with LZ4, not gzip

**Decided.** Field datasets are chunked and compressed with LZ4 through the registered HDF5 filter, not with
HDF5's built-in gzip.

**What it costs.** The contract used to promise files "readable by any HDF5 tool, including `h5py`, without
abm-io". That promise is now narrower: a reader needs the standard filter plugins, which `pip` and
conda-forge both package, rather than needing nothing at all.

**Why.** At the sizes these files reach, storage is not the constraint; the time spent moving and
transforming bytes is. LZ4 compresses and decompresses several times faster than gzip and compresses less,
which is the right trade when a checkpoint is written every simulated hour and read on every restart. A
writer can still be asked for gzip where a file must reach a reader that cannot install a plugin.

**What it does not cost.** abm-io needs no plugin itself: it registers the filter with `H5Zregister`,
implemented with the LZ4 it already links, so its own reads and writes go through HDF5's ordinary paths.
Tested before the design was fixed: a file written that way is read correctly by `h5py` with
`hdf5plugin`, so the bytes are the registered filter's, not ours (see `docs/FINDINGS.md`). The dependency
falls on third-party readers, not on this library or its build.

An earlier version of this entry said abm-io would hand HDF5 pre-compressed chunks through the direct
chunk write. That works, but it requires the filter to be registered anyway for the dataset to be created
with a mandatory filter, so registering it and using the ordinary paths is simpler and was adopted
instead.

**What would change our mind.** Evidence that the plugin is a real obstacle for the people who read these
files, or a measurement showing the compression step is not on the critical path after all, in which case
the stronger promise would be worth more than the speed.

---

## 9. Whether to compress is decided, not switched

**Decided.** The checkpoint writer takes `off`, `on` or `auto`. `auto` decides from declared inputs: the
storage bandwidth a site configures or a short calibration measures once at start-up, the compressor's
measured rate, and the ratio seen on the first chunks. The decision and its inputs are written into the
file.

**Why there is a decision at all.** Compressing is not free. Our measured gather bound is 27 to 55 ns per
agent, so a million agents of roughly 120 bytes each are read in about 55 ms, an effective 2 GB/s. LZ4
compresses that 120 MB at perhaps 500 MB/s on one core, four times longer than the gather, and the
compressed 15 MB then costs almost nothing to write. On storage faster than the compressor, compressing
therefore makes checkpointing slower, by roughly a factor of two on local NVMe; on a shared file system
giving a node a few hundred megabytes per second, it makes it faster. The crossover sits near a gigabyte
per second at our ratios, which is exactly the range separating a laptop from a cluster. Neither answer is
right everywhere, so neither is a good default to hard-code.

**Alternatives rejected.**

- *A manual switch alone.* It asks the user to know their file system's bandwidth and the compressor's
  rate, which few do, and to revisit it whenever either changes. The switch remains, as `off` and `on`,
  for those who do know.
- *Deciding from live timings.* Tempting, and wrong: two runs of the same simulation on the same machine
  would write different files depending on load, which is a poor property to debug or audit against.
  Declared inputs keep the decision reproducible, and recording them keeps it explicable.

**What it must never do, and this is proved rather than intended.** The decision cannot change the data:
two checkpoints of the same simulation written with different decisions restore identically. That is what
makes the policy safe to retune later, by us or by a site, without anyone re-examining whether the science
still holds.

**What the proofs cover, and what they do not.** The rates are proved over 32 bits, which is every rate
these files will meet, 4 GB/s and below. The reason is instructive: the rule multiplies two of its inputs,
and a bit-blasting solver builds a multiplier as wide as the words it is given, so 64 symbolic bits on both
sides does not finish in fifty minutes while 32 does. Nonlinear arithmetic over wide words is a different
wall from the path explosion that bounded the decoder proofs, and worth knowing before designing the next
thing to be proved.

**What would change our mind.** A measurement placing the crossover somewhere else, which would move the
rule rather than remove it. Or evidence that HDF5's filter pipeline, which compresses on one thread per
process, is the real ceiling: then the answer is not a better policy but compressing chunks on several
threads and handing them over with the direct chunk write, which was prototyped and works.
## 10. A checkpoint becomes visible only when it is complete

**Decided.** Parts are written to temporary names and renamed; a manifest listing them is written last and
renamed; the pointer to the newest checkpoint is replaced last of all. A reader sees the previous complete
checkpoint or the new one, never a mixture.

**Why, with numbers rather than argument.** A hundred writers were killed at random moments during a
checkpoint. Written in place, with the pointer moved first, the reader saw a complete checkpoint 15 times,
a partial one 81 times, and 4 times a file that was listed and could not be opened. With the manifest, 100
times out of 100 it saw a complete checkpoint. The full measurement is in `docs/FINDINGS.md`.

The four corrupt cases are the reason this is not a refinement. A partial checkpoint announces itself; a
corrupt one is found only when a job has queued, started and taken its nodes.

**Where the idea comes from.** The data-lake table formats, which solved the same problem for the same
reason: many writers, no transactions, and readers who must never see a half-written state. This is the one
idea from the database world that transfers to our write path intact. The rest of that machinery, logging,
locking, multi-version visibility, exists to resolve conflicts between transactions, and a checkpoint has
none: each rank owns its agents and nobody updates another's.

**What it costs.** One rename per part, one small file, one more rename. Nothing measurable against the
write itself.

**What would change our mind.** A storage system where rename is not atomic, which is most object stores:
there the same guarantee needs a conditional write or a versioned pointer instead, and the experiment should
be repeated against one before any cloud backend is trusted. That is why the rule is stated as "becomes
visible only when complete" rather than as "rename the manifest": the property is what matters, and the
mechanism for achieving it will differ per backend.

---

---

## What this episode taught about the documents

Two of the objections were misreadings, and both are our fault:

- the per-process checkpoint decision reads as "one file per rank, forever", because the scaling path lives
  in a different document;
- the exchange reads as if variable-length data were crammed into the arenas, because the contract never
  says plainly that it takes another path.

A critique that misreads a document twice has found two places where the document is unclear. Both are now
fixed in `INTEGRATION.md`, which says where per-process files stop scaling and what replaces them, and
states plainly that variable-length data does not travel through the arenas.

## Where this came from

An analysis of the public repository, forwarded by a user in November 2026 and probably machine-written.
Its origin was deliberately not weighed: a design argument is right or wrong on its own terms, and this one
was right about three things, including the one that mattered most.
