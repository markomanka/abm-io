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
