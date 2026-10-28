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
| Schema | Describes agent and behaviour types: their fields, field types, units and roles | To build (this contract) |
| Codecs | Packs fields into rows or columns; delta encoding against a previous frame; compression | Prototype, tested and fuzzed |
| Exchange | Packs and unpacks agents for neighbouring processes into receiver-owned memory (arenas) | Prototype, tested with MPI |
| Containers | Checkpoint files (HDF5) and visualization files (VTKHDF) | To build |
| Adapters | Connect a simulator: BioDynaMo (generated), PhysiCell (hand-written) | To build |

Each layer uses only the layers above it. A simulator can use the schema and containers without the exchange
layer (PhysiCell runs on shared memory), or the exchange without containers.

## The schema

### Types

Every agent or behaviour type has a **type description**:

- a **qualified name**, such as `bdm::Cell`, unique within a simulator;
- a **type identifier**: a 64-bit hash of the qualified name, so it is stable across runs, processes and machines
  without a central registry;
- an optional **base type**, whose fields come first;
- a **schema version**, incremented when the fields change;
- the ordered list of **fields**.

### Fields

| Property | Meaning |
| --- | --- |
| Name | Unique within the type, including its base types |
| Kind | Scalar (integer, floating point, boolean), fixed-size array (such as a 3D position), variable-size array, string, reference to another agent, nested record, or list of polymorphic records (such as behaviours) |
| Element type and count | For example `float64 × 3` |
| Unit | Optional, for example `um` or `min`; carried into files and used for MultiCellDS mapping |
| Roles | Any combination of *checkpoint*, *exchange* and *export*; a field without roles is never stored or sent |

Roles let one description serve every purpose. A position is needed everywhere; a cached neighbour list is
transient and has no role; a field used only for analysis can be *export* without being sent between processes.

### How a simulator describes its types

The adapter provides, for each type, a **visitor function**: a template that calls the visitor once per field,
in declaration order, with the field's name, a reference to the member and its roles. The library's visitors
implement packing, unpacking, writing, reading and comparison on top of that single function. There is no
virtual call per field, and the compiler can inline everything.

- **BioDynaMo:** the generator `abm-gen` reads the model's C++ headers (types marked with BioDynaMo's
  `BDM_AGENT_HEADER` and `BDM_BEHAVIOR_HEADER`) and emits the visitor functions at build time. Members marked
  transient in the source (`//!`) get no role. User models need no changes.
- **PhysiCell:** a hand-written adapter describes `Cell` and its phenotype, and turns PhysiCell's custom cell
  data, which already carries names and units, into fields.

### References and polymorphism

- A reference to another agent is stored as that agent's **unique identifier**, never as a memory address.
  After loading or receiving, the simulator resolves identifiers back to agents.
- Each stored object carries its type identifier. Lists of behaviours are stored as lists of
  (type identifier, record) pairs, so different behaviour types can share one list.

### Schema evolution

Every file and every exchange stream carries the schemas it was written with. Readers match fields **by
name**: a field missing from the file takes its default value, a field unknown to the reader is skipped, and a
field whose type changed needs a converter registered for that schema version. Old files therefore stay
readable after a model changes.

## Checkpoint files

Checkpoints are HDF5 files, readable by any HDF5 tool, including `h5py`, without abm-io.

```
/                      attributes: format version, producer (simulator, version), creation time
/schema                the type descriptions, as JSON
/simulation            attributes: simulated time, step, time unit; simulator parameters as JSON
/agents/<type>/uid     one entry per agent of that type
/agents/<type>/<field> one dataset per field, column by column, chunked and compressed
/environment/<name>    continuum fields (for example substance concentrations): values, origin, spacing, unit
/random                state of every random stream (generator key and counter)
/host/<simulator>      anything else the simulator needs to resume, in its own layout
```

- **Restart guarantee:** restoring a checkpoint reproduces every field with the *checkpoint* role exactly, and
  a simulation continued from a checkpoint matches the uninterrupted run, given the same random streams.
- **Parallel runs:** each process writes its own agents. With parallel HDF5 the processes write one shared file
  collectively; without it, one file per process plus an index file. The reader accepts both.

## Exchange between processes

The simulator decides which agents go to which neighbouring process, either because they move there or because
they are needed as read-only copies near a border. abm-io then:

1. packs the fields with the *exchange* role into rows or columns;
2. optionally encodes the difference from the previous frame sent to the same neighbour;
3. optionally compresses, which pays off on slow links;
4. unpacks on the receiving side into memory that the receiver owns and reuses, so the steady state allocates
   nothing per agent.

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

## Open questions for review

1. **Type identifiers:** a hash of the qualified name (proposed) or numbers assigned by a registry?
2. **Parallel checkpoints:** require parallel HDF5, or always support one file per process as well (proposed)?
3. **The generator:** Python with Clang's parser (`libclang`, packaged on conda-forge; proposed), or C++ with
   Clang's LibTooling?
4. **Where the generator lives:** in this repository (proposed), so its output and the library change together?
5. **Units:** record units as metadata only (proposed), or convert to a common system on writing?

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
