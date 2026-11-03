# Learned operations and live data: what to keep possible

A design note, not a plan. Nothing here is scheduled, and none of it belongs in the milestones of
[ROADMAP.md](ROADMAP.md) yet. Its purpose is to record, while the decisions are still cheap, what abm-io
would need in order to carry two things it does not carry today:

- **learned operations**: a part of a simulation computed by a trained model instead of by a solver, to make
  a simulation faster, or to reach detail a solver cannot afford;
- **live data**: measurements from an experiment flowing into a running simulation, and the simulation
  proposing what to measure next.

Together these are what a *digital twin engine* needs: a model that is corrected by observation rather than
only compared with it afterwards.

## What the field already learned, and what transfers

High-energy physics has done this for years, and its published conclusions are the useful part.

Generative models reproduced calorimeter showers with about 10 % accuracy and a speedup of more than six
orders of magnitude over Geant4 (Vallecorsa, *J. Phys. Conf. Ser.* 1085, 022005, 2018). The remaining work,
as the same group lists it, is "production level, a validation process, systematics, integration in
simulation frameworks, out-of-distribution generalisation" (Vallecorsa, CERN openlab, 2021). A later full
quantum GAN of the same lineage reproduces the average shower image but states plainly that it "cannot
reproduce the more sophisticated physics accuracy metrics beside the average shower image, for example the
pixel-wise correlation patterns" (Rehm et al., arXiv:2305.07284).

Two lessons transfer:

1. **The model is the tractable part; validation and integration are not.** Whatever we do, the engineering
   that decides whether a learned component is usable is the contract it is held to, not its architecture.
2. **Averages are the wrong acceptance criterion for an agent-based model.** A surrogate that matches a
   growth curve while getting neighbour correlations wrong has failed, because in an agent-based model the
   correlations are the science. Acceptance must be stated on spatial statistics, lineage structure and
   distributions, not on means.

## Where a learned operation could sit

In rough order of how unambiguously it can be judged:

| What it replaces | Why it is attractive | How it would be judged |
| --- | --- | --- |
| A field solver, diffusion first | Called every step; the dominant cost in many models | The benchmark's diffusion unit test, against its analytical solution |
| Sub-grid mechanics: a deformable-cell response applied to centre-based agents | Buys back detail the centre-based trade-off gives up | Against a deformable-cell simulation on the same configuration |
| Initial conditions: sampling tissue architectures | Realistic starting states without hand-built geometry | Spatial statistics against imaging data |
| Calibration: a learned posterior over parameters given observations | Turns a generic model into a twin of one sample | Coverage of the posterior, and prediction on held-out data |
| Coarse-graining, with learned closures | The only route to whole-organ scales (see the design reflection) | Against a fine-grained simulation of a sub-volume |

The first row is the one to attempt first, whenever that day comes: the input and output are well defined,
the reference is a numerical solution, and the acceptance test already exists in the community benchmark.

## Live data: assimilation and experiment design

Data flowing inward is **state estimation**: the simulation carries an ensemble of candidate states, each is
compared with the measurements, and the ensemble is reweighted or resampled. Data flowing outward is
**experiment design**: the simulation says which stimulus or measurement would most reduce its uncertainty.
A loop of the two is what distinguishes a twin from a model.

This needs, beyond what a simulator normally has:

- an **observation operator** per instrument: a function from model state to predicted measurement, with
  the units and the uncertainty of that prediction;
- **time alignment**: measurements carry their own clock, and a latency budget if the loop is live;
- **ensembles**: many states advanced together, forked and restored cheaply;
- **provenance**: which measurements, which model version, which weights produced a given state.

## What abm-io already provides

Much of the substrate exists, which is why this note is cheap to write now:

- **The schema with units** is the vocabulary an observation operator needs: a reading in µV or µM can only
  be compared with a field that declares what it is.
- **Global agent identifiers, never reused**, let an observation refer to the same agent across time, ranks
  and restarts.
- **Checkpoints and delta encoding** are what an ensemble runs on: cheap fork, cheap restore, and
  incremental state rather than whole copies.
- **Counter-based random numbers** make ensemble members reproducible and independently seeded, which is
  the difference between a debuggable filter and a mystery.
- **Exchange as a plug-in point** has the shape a learned operation needs: named columns in, named columns
  out, at its own cadence.
- **The community benchmark** (Ntiniakou et al., bioRxiv 10.1101/2025.07.16.664358) is a ready-made
  certification harness: a learned diffusion surrogate must pass the same unit test the five engines passed.

## What would have to be added

Four things, none of them urgent, all of them cheaper to allow for now than to retrofit.

1. **Declared inputs and outputs for every operation.** An operation that states which fields it reads and
   which it writes can be replaced by a learned one without the engine knowing the difference. The schema
   already describes fields and their roles; what is missing is the same discipline for operations. This is
   the one decision worth keeping in mind while the rest of abm-io is written.
2. **A surrogate contract.** A learned operation declares its inputs and outputs as above, plus a **validity
   domain** (the range of states it was trained for) and an **uncertainty output**. A surrogate that cannot
   say when it is extrapolating cannot be trusted in a loop, and the engine must be able to fall back to the
   solver when it says so.
3. **An observation contract.** The counterpart for data coming in: the instrument, the fields it observes,
   the operator that predicts the measurement from state, the units, and the uncertainty.
4. **Weights are data, not code.** A learned component is identified by the content of its weights, and a
   run records which weights it used, as it records its parameters and its seed. Otherwise a result cannot
   be reproduced even with the same source.

## What this note deliberately does not propose

- No dependency on any machine-learning library, now or as part of this idea. A learned operation runs
  outside abm-io, communicating through the formats abm-io already defines.
- No quantum computing. The quantum generative models in this lineage work on eight-pixel images, on
  simulators; their interesting claim is parameter efficiency, not readiness.
- No schedule. When any of this is attempted, it starts with the diffusion surrogate and the benchmark's own
  test, and it is judged on correlations rather than on averages.

## References

- S. Vallecorsa, "Generative models for fast simulation", *J. Phys.: Conf. Ser.* 1085, 022005 (2018).
- S. Vallecorsa, "ML/AI methods for calorimeter simulation", CERN openlab, 9 November 2021.
- F. Rehm, S. Vallecorsa, M. Grossi, K. Borras, D. Krücker, "A Full Quantum Generative Adversarial Network
  Model for High Energy Physics Simulations", arXiv:2305.07284v2 (2024).
- T. Ntiniakou et al., "Open Benchmarking for Cell-Based Multiscale Models: Lessons from a Community
  Initiative", bioRxiv 10.1101/2025.07.16.664358 (2025).
