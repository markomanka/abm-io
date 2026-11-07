# abm-io integration contract

**Status:** draft for review, October 2026. Once agreed, this document becomes the specification that the
headers, the generator and the adapters implement.

## Why abm-io exists

Agent-based simulators of multicellular biology, such as BioDynaMo and PhysiCell, keep facing the same
problems outside their core algorithms:

- **Checkpoint and restart.** Long simulations need to stop and resume exactly. In the community
  benchmark of cell-based simulation tools [1], neither BioDynaMo nor PhysiCell is listed as supporting
  checkpointing (Table S1 of [1]).
- **Consistent, comparable output.** The same benchmark found that comparing tools needed substantial work just
  to align inputs and outputs, and called for "common function signatures, consistent I/O, and easy comparison
  set-ups" and for interoperability standards such as MultiCellDS [2].
- **Scaling on parallel hardware.** When a simulation is distributed across processes, agents near the
  borders between processes must be exchanged at every iteration, and that exchange can dominate the
  run time. TeraAgent [3] showed that serialization tailored to the agent model, and delta encoding that
  exploits how little agents change from one iteration to the next, make simulations of half a trillion
  agents possible.

abm-io provides these capabilities once, as a library that each simulator connects to through a thin adapter.
It follows principles published in [1] and [3]; it shares no code with the tools described there.

## Scope

**In scope:** describing agent types (schemas), checkpoint files, exchanging agents between processes, and
writing files for visualization.

**Out of scope:** simulation algorithms (mechanics, diffusion, cell cycles), scheduling, and decisions about
which agents move where. The simulator decides *what* to save, send or show; abm-io does it efficiently and
in a documented format.

## Architecture

| Layer | Responsibility | State |
| --- | --- | --- |
| Schema | Describes agent and behaviour types: their fields, field types, units and roles | In progress: descriptions, identifiers, the visitor protocol (`include/abmio/schema.h`) and the generator `abm-gen`; its validation on BioDynaMo next |
| Codecs | Packs fields into rows or columns; delta encoding against a previous frame; compression | Prototype, tested and fuzzed |
| Exchange | Packs and unpacks agents for neighbouring processes into receiver-owned memory (arenas) | Prototype, tested with MPI; packs the fields with the exchange role through the visitor protocol |
| Containers | Checkpoint files (HDF5) and visualization files (VTKHDF) | To build |
| Adapters | Connect a simulator: BioDynaMo (generated), PhysiCell (hand-written) | To build |

Each layer uses only the layers above it. A simulator can use the schema and containers without the exchange
layer (PhysiCell runs on shared memory), or the exchange without containers.

## The schema

### Types

Every agent or behaviour type has a **type description**:

- a **qualified name**, such as `bdm::Cell`, unique within a simulator;
- a **type identifier**: by default the 64-bit FNV-1a hash of the qualified name, so it is stable across runs,
  processes and machines without a central registry, and any tool computes it the same way. A type description
  may instead fix its identifier explicitly, which keeps it stable when the type is renamed; 0 means *no type*;
- an optional **base type**, whose fields come first;
- a **schema version**, incremented when the fields change;
- the ordered list of **fields**.

### Fields

| Property | Meaning |
| --- | --- |
| Name | Unique within the type, including its base types |
| Kind | Scalar, fixed-size array (such as a 3D position), variable-size array, string, nested record, or list of polymorphic records (such as behaviours) |
| Element type and count | For example `float64 × 3`: integers, floating point, booleans, characters, or agent identifiers for references to other agents |
| Stored as | Optional narrower element type of the same category for files and streams, such as `float32` for a `float64` member; precision only, never units |
| Unit | Optional, for example `um` or `min`; carried into files unchanged, never converted, checked for consistency when loading, and used for MultiCellDS mapping |
| Roles | Any combination of *checkpoint*, *exchange* and *export*; a field without roles is never stored or sent |
| Ontology term | Optional, for example from the Cell Ontology or the Relation Ontology; types and relations carry one too |

Roles let one description serve every purpose. A position is needed everywhere; a cached neighbour list is
transient and has no role; a field used only for analysis can be *export* without being sent between processes.

### How a simulator describes its types

The adapter specialises `abmio::TypeTraits` for each type: the qualified name, the schema version, the base
type, and a **visitor function**, a template that calls the visitor once per field, in declaration order, with
the field's name and properties (roles, unit, term, stored type) and a **pointer to the member**. Pointers
rather than references let the library describe a type without constructing an object (a BioDynaMo agent
cannot be constructed outside a running simulation), while its data visitors bind the object themselves. The
library's visitors implement description, packing, unpacking, writing, reading and comparison on top of that
single function. There is no virtual call per field, and the compiler can inline everything.

- **BioDynaMo:** the generator `abm-gen` reads the model's C++ headers (types marked with BioDynaMo's
  `BDM_AGENT_HEADER` and `BDM_BEHAVIOR_HEADER`) and emits the visitor functions at build time. Members marked
  transient in the source (`//!`) are left out. User models need no changes. Its design is in
  `docs/GENERATOR.md`.
- **PhysiCell:** a hand-written adapter describes `Cell` and its phenotype, and turns PhysiCell's custom cell
  data, which already carries names and units, into fields.

A simulator's own member types, such as BioDynaMo's `Real3` or `AgentPointer`, are declared once in its
adapter by specialising `abmio::FieldTraits`; descriptions and data visitors then handle them like built-in
types.

### References and polymorphism

- A reference to another agent is stored as that agent's **unique identifier**, never as a memory address.
  After loading or receiving, the simulator resolves identifiers back to agents.
- Each stored object carries its type identifier. Lists of behaviours are stored as lists of
  (type identifier, record) pairs, so different behaviour types can share one list.

### Agent identifiers

Every agent carries a 64-bit identifier, unique across processes for the whole run; 0 means *no agent*. abm-io
never reuses identifiers: process *r* of *n* takes *first + r*, *first + r + n*, and so on, without coordination.
After a restart, *first* is one more than the largest identifier in the checkpoint, so the number of processes
may change. A simulator may reuse identifiers at its users' choice, provided a generation counter keeps them
unique.

### Relations

Persistent relations between agents, such as adhesion or synapses, are **edge tables**: a source and a target
agent identifier per edge, followed by attribute fields. A relation has a name and an optional ontology term.
Lineage needs no edge table: a parent reference is a column of the agent type.

### Schema evolution

Every file and every exchange stream carries the schemas it was written with. Readers match fields **by
name**: a field missing from the file takes its default value, a field unknown to the reader is skipped, and a
field whose type changed needs a converter registered for that schema version. Old files therefore stay
readable after a model changes.

**Resuming a simulation is strict.** That permissive rule is right for reading an old file for analysis and
wrong for restarting from it: a parameter added since the checkpoint was written would silently take a
default, and the simulation would continue with a value nobody chose. So a checkpoint whose schema differs
from the running model in anything carrying a *checkpoint* role fails to load, naming what differs, unless
the caller explicitly accepts the defaults. Reading the same file for analysis stays permissive.

## Checkpoint files

Checkpoints are HDF5 files, readable without abm-io by any HDF5 tool that has the standard filter plugins
(`hdf5plugin` for Python, which `pip` and conda-forge both provide). See *Compression* below.

```
/                      attributes: format version, producer (simulator, version), creation time
/schema                the type descriptions, as JSON
/agents/<type>/uid     one entry per agent of that type
/agents/<type>/<field> one dataset per field, column by column, chunked and compressed
/environment/<name>    continuum fields (for example substance concentrations): values, origin, spacing, unit
/records/<type>/<field>  one dataset per field, with one row, for the types below
/random                a table: one row per random stream (generator key and counter)
/components            a table: what this run depended on (see Provenance)
/lineage               a table: every accepted discontinuity, carried by every later checkpoint
/host/<simulator>      anything else the simulator needs to resume, in its own layout
```

### Tables and records

A checkpoint holds **tables**, many instances of a described type, and **records**, exactly one. The two are
the same thing: a record is a table with one row. Both are described in `/schema` like any other type, so
both carry a version, both evolve by the rules above, and both are covered by the strict resume. There is
no second set of rules for "everything that is not an agent", which is what a simulation's own state,
written in attributes, would have become.

abm-io defines five types for what every simulation has; a simulator adds its own as it likes, and they are
read back the same way:

| Type | Shape | Holds |
| --- | --- | --- |
| `abmio.Simulation` | record | the step, the simulated time and its unit |
| `abmio.RandomStream` | table | one row per stream: its key, its counter, what it belongs to |
| `abmio.Provenance` | record | what wrote this checkpoint: abm-io's version, the schema's digest, the seed, the compression decision and the inputs it was made from |
| `abmio.Component` | table | one row per thing the run depended on (below) |
| `abmio.Lineage` | table | one row per accepted discontinuity: where a resume went ahead although something changed |

The cost is nothing worth measuring: a few rows beside millions of agents.

### Provenance: what a run depended on

A result is only reproducible if what produced it is known. `/components` is a table with one row per
dependency, and the row is the same shape whatever the dependency is:

| Field | Meaning |
| --- | --- |
| `name` | what it is, for example `biodynamo`, `libroadrunner`, `the gradient solver`, an instrument |
| `version` | its version as it reports it |
| `role` | what it did in this run: an open vocabulary, with `simulator`, `solver`, `library`, `model`, `surrogate`, `instrument` recommended |
| `digest` | a content hash where one can be had: of a library, of a model file, of a learned model's weights |
| `affects_results` | whether a change to it changes what the simulation computes |
| `reference` | optional: a DOI, a URL, a commit |

`affects_results` is what makes this load-bearing rather than decorative. **The strict resume compares the
components as well as the schema**: resuming with a different version of a component marked as affecting
results fails and says which one changed. A visualisation library that moved on does not stop a restart; a
gene-network solver that moved on does. A component counts as affecting results if it is declared to on
either side, so a new version cannot slip through by changing its declaration at the same time.

Failing is not refusing. A solver often changes because it was wrong before, and a run resumed on the
corrected one is better science even though it is no longer the same run. So the resume can go ahead, under
three conditions that make the cost visible rather than hidden:

- **Acceptance is per component, by name.** `BlockingDifferences` takes the components the caller accepts,
  not a flag meaning everything, so going ahead means naming what changed. Names are compared exactly:
  accepting `libroad` accepts nothing about `libroadrunner`. An application asks a person at
  this point; the library only reports.
- **The discontinuity is recorded, permanently.** Each accepted change adds a row to `/lineage`: the step,
  the component, the versions before and after, who accepted it and why. Every checkpoint written
  afterwards carries those rows, so a result states its own discontinuities instead of appearing to be
  something it is not.
- **Nothing is silent.** A run with lineage rows is reproducible from its checkpoints and its lineage
  together, and is not reproducible from the original seed alone. Both facts are in the file.

The shape is deliberately one repeated row rather than a field per kind of dependency, so that a kind
nobody has thought of yet needs no change to the format: a learned operation's weights, a feed of
experimental data, a table of parameters fitted elsewhere are all rows with a role and a digest. See
[LEARNED-OPERATIONS.md](LEARNED-OPERATIONS.md), where recording weights as data rather than as code is one
of the things that note asks for, and which this provides.

- **Compression: LZ4, by the registered HDF5 filter.** Where field datasets are compressed, they are
  compressed with LZ4 rather than with HDF5's built-in gzip: LZ4 compresses and decompresses several times
  faster while compressing less, which is the right trade when a checkpoint is written every simulated hour
  and read on every restart. The price is the promise above: a reader needs the standard LZ4 filter plugin,
  which is a package away, rather than nothing at all. A writer may be asked for gzip instead where a file
  must reach a reader that cannot install one.
- **Whether to compress is decided, not assumed.** Compressing is not free: on storage faster than the
  compressor, it makes writing slower, and the crossover sits in the range that separates a laptop's disk
  from a shared parallel file system. The writer therefore takes `off`, `on` or `auto`.

  `auto` decides from **declared inputs**, never from live timings: the storage bandwidth the site
  configures or that a short calibration measures once at start-up, the compressor's measured rate, and the
  ratio observed on the first chunks. Deciding from live timings would make two runs of the same simulation
  produce different files, which is a poor property to debug against. **The decision and the inputs it was
  made from are recorded in the file**, so any run can be explained afterwards.

  The defaults are provisional until measured on real systems, and `docs/FINDINGS.md` records what they rest
  on at any time.
- **abm-io provides the filter itself**, with `H5Zregister`, implemented with the LZ4 it already links, so
  it reads and writes these files through HDF5's ordinary paths and needs no plugin of its own. The format
  is the registered filter's, so a reader with the plugin decodes the same bytes: the total decoded size as
  a big-endian 64-bit integer, the block size as a big-endian 32-bit integer, then each block as its
  big-endian 32-bit compressed size followed by its bytes. The plugin is a requirement on third-party
  readers, not on this library or its build.
- **Restart guarantee:** restoring a checkpoint reproduces every field with the *checkpoint* role exactly, and
  a simulation continued from a checkpoint matches the uninterrupted run, given the same random streams.
- **Compression is semantically invisible.** Whatever the compression decision, restoring a checkpoint gives
  the same state: two checkpoints of the same simulation written with different decisions restore
  identically. The policy can therefore be retuned, by us or by a site, without anyone re-examining whether
  the science still holds. This is a proof obligation, not a hope; see milestone R.
- **Memory while writing is bounded by the chunk, not by the population.** The writer walks the agents in
  blocks of one chunk and fills every column's buffer in that single pass, so its peak is proportional to
  *fields × chunk size* and does not grow with the number of agents: the same tens of megabytes at a hundred
  thousand agents and at a billion. One pass over the agents per block, not one pass per field. HDF5's chunk
  cache is sized to hold at least one chunk per open dataset, since a smaller cache turns every partial
  chunk into a read, a modify and a write.
- **Parallel runs:** each process writes its own agents. One file per process plus an index file is always
  available; with parallel HDF5, the processes may instead write one shared file collectively. The reader
  accepts both.
- **A checkpoint becomes visible only when it is complete.** Each writer writes to a temporary name and
  renames it, so a file never appears half-written. When every writer has finished, a **manifest** listing
  the parts is written to a temporary name and renamed, and only then is the pointer to the newest
  checkpoint replaced, by rename as well. A reader therefore sees either the previous complete checkpoint or
  the new one, never a mixture, whatever moment a job is killed at. Measured: see
  [FINDINGS.md](FINDINGS.md). The property is stated rather than the mechanism because rename is atomic
  on POSIX file systems and is not on most object stores;
  [STORAGE-BACKENDS.md](STORAGE-BACKENDS.md) records what would achieve it elsewhere.
- **At large process counts, writers are grouped.** One file per process is the form that must always work,
  not the form to use at any scale: on a parallel file system, tens of thousands of files per checkpoint
  become a metadata bottleneck, and closing them can cost more than the step that produced them. Writers are
  then grouped, one file per node or per some hundreds of ranks, which is the same arrangement HDF5 subfiling
  and ADIOS2 provide. Cloud object storage pulls the other way, favouring many independent objects, which is
  another reason the per-process form is the one that must always work. Where the crossover lies is a
  measurement nobody has made here yet.

## Exchange between processes

The simulator decides which agents go to which neighbouring process, either because they move there or because
they are needed as read-only copies near a border. abm-io then:

1. packs the fields with the *exchange* role into rows or columns;
2. optionally encodes the difference from the previous frame sent to the same neighbour;
3. optionally compresses, which pays off on slow links;
4. unpacks on the receiving side into memory that the receiver owns and reuses, so the steady state allocates
   nothing per agent.

Only fixed-size fields travel that way, as elements in rows or columns. **Variable-length data takes another
path**: vectors, strings, nested records and the lists of behaviours are written after them, with their
counts and offsets in the message, because a reusable arena suits fixed-size data and nothing else. The two
are separate by design, not crammed together.

Decoders validate every frame, and fuzzing covers them. The library produces and consumes byte buffers; an
optional module sends them over MPI on a communicator supplied by the simulator, so abm-io never assumes
it owns MPI.

## Files for visualization

From the same schema, abm-io writes **VTKHDF**, the HDF5-based VTK file format: agents as point data (position,
size and every field with the *export* role) and continuum fields as image data. ParaView, PyVista and other
VTK readers open these files directly, so neither the simulator nor abm-io needs to link a visualization
library.

## Performance contract

- The cost of packing an agent is measured against the **gather bound**: the time it takes just to read the
  fields involved. The prototype stays within 1.25–1.55× of that bound; the library must stay within 1.6×.
- Steady-state exchange performs no per-agent allocation and no virtual call per field.
- Every change runs the benchmarks on a dedicated machine, and a slowdown beyond the agreed tolerance blocks it.
- Checkpoint writing should approach the bandwidth of the file system, and is measured the same way.

## Testing contract

- Round-trip tests for every field kind, in rows and columns, with and without delta encoding.
- Fuzzing of every decoder, on every pull request and in longer scheduled runs.
- Multi-process tests of the exchange.
- Schema-evolution tests: files written by an older schema version must load.
- In each simulator, through its adapter: saving and restoring must reproduce the state exactly, and a run
  continued from a checkpoint must match the uninterrupted run.

## Versioning

The library follows semantic versioning. File formats carry their own version, independent of the library's,
and readers keep supporting older format versions.

## Decisions

The questions this contract left open for review, settled in October 2026:

1. **Type identifiers:** a 64-bit hash of the qualified name, with no registry. Simulators, and implementations
   plugged in from outside this repository, can define types without coordinating with anyone. A type
   description may fix its identifier explicitly, so a renamed type keeps reading old files. With 64 bits,
   collisions are negligible; a reader that finds two types with the same identifier reports it and stops.
2. **Parallel checkpoints:** one file per process plus an index file is always supported; a single shared file
   through parallel HDF5 is optional, never required. This avoids depending on parallel HDF5, maps directly onto
   the object storage of commercial clouds, and avoids a single-file bottleneck at large scale.
3. **The generator:** Python with Clang's parser (`libclang`, packaged on conda-forge). It gives the full C++
   parse, and Python is quicker to write and maintain than LibTooling, and the language of the planned analysis
   and front-end tools.
4. **Where the generator lives:** in this repository, so its output and the library change together.
5. **Units:** recorded as metadata only, never converted on writing, so files keep exactly what the simulator
   computed. Readers, and the model descriptions planned for multi-simulator studies, check units for
   consistency when loading.

## Keeping later ideas possible

Two things abm-io does not do today would be expensive to retrofit and cheap to allow for: a part of a
simulation computed by a trained model instead of a solver, and measurements from a running experiment
flowing into a simulation. [LEARNED-OPERATIONS.md](LEARNED-OPERATIONS.md) records what each would need. The
one point that bears on this contract now: an operation that declares which fields it reads and which it
writes can be replaced by a learned one without the engine knowing the difference, so operations deserve the
same discipline as fields.

## References

1. Ntiniakou T, Osborne J, Zhao J, et al. *Open Benchmarking for Cell-Based Multiscale Models: Lessons from a
   Community Initiative.* bioRxiv 2025.07.16.664358 (2025). https://doi.org/10.1101/2025.07.16.664358
2. Friedman SH, Anderson ARA, Bortz DM, et al. *MultiCellDS: A community-developed standard for curating
   microenvironment-dependent multicellular data.* bioRxiv 090456 (2016). https://doi.org/10.1101/090456
3. Breitwieser L, Hesam A, Yağlıkçı AG, Sadrosadati M, Rademakers F, Mutlu O. *TeraAgent: A Distributed
   Agent-Based Simulation Engine for Simulating Half a Trillion Agents.* arXiv:2509.24063 (2026); accepted
   for publication in IEEE Access. https://arxiv.org/abs/2509.24063
4. Breitwieser L, Hesam A, de Montigny J, et al. *BioDynaMo: a modular platform for high-performance agent-based
   simulation.* Bioinformatics 38(2):453–460 (2021). https://doi.org/10.1093/bioinformatics/btab649
5. Ghaffarizadeh A, Heiland R, Friedman SH, Mumenthaler SM, Macklin P. *PhysiCell: An open source physics-based
   cell simulator for 3-D multicellular systems.* PLOS Computational Biology 14(2):e1005991 (2018).
   https://doi.org/10.1371/journal.pcbi.1005991
