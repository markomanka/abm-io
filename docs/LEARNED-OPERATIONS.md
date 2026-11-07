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

## How these things fail

The failure modes are better documented than the successes, and they decide what a contract must demand.

**Error accumulates over a rollout.** A surrogate judged on one step can drift over many. The arithmetic is
unforgiving: a solver on a 0.01-minute step, the benchmark's diffusion resolution, is called about 1.9
million times in a thirteen-day simulation. A bias far too small to see in a single-step comparison is not
small after that, and a surrogate that is slightly unstable will diverge. **Acceptance must therefore be
measured on long rollouts against the solver, not on single steps**, and the rollout must be as long as the
simulations the surrogate is meant to serve.

**Invariants are not preserved unless they are imposed.** A diffusion solver conserves mass up to its
boundary flux; a learned operator conserves nothing by construction, so it can create or destroy the
substance a model's biology depends on. Either the surrogate is built to preserve the invariant, or its
output is projected back onto it, or at the very least the invariant is checked at run time. Counters over
totals cost almost nothing and catch this immediately.

**Baselines are routinely too weak.** The machine-learning literature on solver surrogates has a documented
reporting bias: speedups are claimed against unoptimised baselines, so a surrogate "ten times faster" than a
naive implementation may be slower than a well-tuned one. Any speedup we report must name the baseline's
configuration: its threads, its vectorisation, its tolerances.

**Out-of-distribution behaviour is silent.** A surrogate asked for a state it was never trained on returns
an answer with the same confidence as any other. This is why a validity domain and an uncertainty output
are not refinements but the minimum, and why the engine must be able to fall back to the solver.

**Averages hide what matters.** Stated above, and worth repeating as an acceptance rule: a surrogate is
judged on spatial statistics, neighbour correlations and lineage structure, because those are what an
agent-based model exists to produce.

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

The data rates settle the shape of the loop before any modelling choice does. A high-density CMOS
microelectrode array of the kind used on neuronal cultures carries tens of thousands of electrodes sampled
at tens of kilohertz, on the order of a gigabyte per second of raw signal (from memory; to confirm against
a current instrument). No agent-based simulation assimilates that stream directly. The loop therefore runs
on **features** at a cadence the simulation can meet, spike times, firing rates, propagation patterns,
extracted near the instrument, while the raw stream is archived separately. The same holds for imaging:
segmented positions and tracks, not pixels. This is a requirement on the observation operator, which maps
model state to a *feature*, not to a raw measurement.

Closed-loop stimulation of neuronal cultures on such arrays is already published work rather than a
prospect, which makes this the most concrete of the directions in this note, and the one where a partner
with an instrument matters more than anything in abm-io.

## Calibration is what makes a twin, and it needs the acceleration

A model becomes a twin of one sample by having its parameters inferred from that sample's data, with
uncertainty. The modern form of this is simulation-based inference: train an estimator of the posterior
over parameters by running the simulator many times. Note the circularity, which is also the opportunity:
inference needs a fast forward model, and a fast forward model is what a surrogate provides. Acceleration
and twinning are the same programme, not two.

Calibration and validation of agent-based models in biomedicine already has a literature of its own, and
one of its reviews is by members of this consortium; the methods it surveys, and the uncertainty
quantification tools the benchmark paper cites, are the standard any learned component would have to meet
rather than bypass.

## Differentiability, if it ever comes for free

If an operation's inputs and outputs are declared, a further step becomes possible later: making the
operation differentiable, so that parameters can be fitted by gradient rather than by search, and a learned
component can be trained inside the simulation rather than beside it. Differentiable agent-based models
exist, mostly in epidemiology. This note proposes nothing of the sort; it records only that the same
declaration that allows an operation to be replaced is what would allow it to be differentiated, which is
another reason the discipline is worth keeping.

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
  High-energy physics went the same way, agreeing community benchmarks and metrics for generative
  surrogates rather than letting each group report its own; the benchmark here could grow such a track, with
  the metrics stated on correlations and the baseline's configuration recorded alongside every speedup.

## What would have to be added

Five things, none of them urgent, all of them cheaper to allow for now than to retrofit.

1. **Declared inputs and outputs for every operation.** An operation that states which fields it reads and
   which it writes can be replaced by a learned one without the engine knowing the difference. The schema
   already describes fields and their roles; what is missing is the same discipline for operations. This is
   the one decision worth keeping in mind while the rest of abm-io is written.
2. **A surrogate contract.** A learned operation declares its inputs and outputs as above, plus a **validity
   domain** (the range of states it was trained for) and an **uncertainty output**. A surrogate that cannot
   say when it is extrapolating cannot be trusted in a loop, and the engine must be able to fall back to the
   solver when it says so.
3. **An observation contract.** The counterpart for data coming in: the instrument, the features it
   observes, the operator that predicts the feature from state, the units, and the uncertainty.
4. **Declared invariants, checked at run time.** A learned operation states which quantities it must
   preserve, and the engine checks them with counters that cost nothing: a surrogate that quietly destroys
   mass is worse than a slow solver.
5. **Weights are data, not code.** A learned component is identified by the content of its weights, and a
   run records which weights it used, as it records its parameters and its seed. Otherwise a result cannot
   be reproduced even with the same source. **This one exists**: a checkpoint's `/components` table holds
   a row per dependency with a name, a version, a role, a content digest and whether it affects results,
   and a learned operation's weights are such a row ([INTEGRATION.md](INTEGRATION.md), "Provenance").

## What this note deliberately does not propose

- No dependency on any machine-learning library, now or as part of this idea. A learned operation runs
  outside abm-io, communicating through the formats abm-io already defines.
- No quantum computing. The quantum generative models in this lineage work on eight-pixel images, on
  simulators; their interesting claim is parameter efficiency, not readiness.
- No schedule. When any of this is attempted, it starts with the diffusion surrogate and the benchmark's own
  test, and it is judged on correlations rather than on averages.

## References

Read while writing this note:

- S. Vallecorsa, "Generative models for fast simulation", *J. Phys.: Conf. Ser.* 1085, 022005 (2018).
- S. Vallecorsa, "ML/AI methods for calorimeter simulation", CERN openlab, 9 November 2021.
- F. Rehm, S. Vallecorsa, M. Grossi, K. Borras, D. Krücker, "A Full Quantum Generative Adversarial Network
  Model for High Energy Physics Simulations", arXiv:2305.07284v2 (2024).
- T. Ntiniakou et al., "Open Benchmarking for Cell-Based Multiscale Models: Lessons from a Community
  Initiative", bioRxiv 10.1101/2025.07.16.664358 (2025).

Cited by that benchmark paper, and the nearest prior art in this field:

- J. Metzcar, C. R. Jutzeler, P. Macklin, A. Köhn-Luque, S. C. Brüningk, "A review of mechanistic learning
  in mathematical oncology", arXiv:2312.07026 (2023).
- N. Cogno, C. Axenie, R. Bauer, V. Vavourakis, "Agent-based modeling in cancer biomedicine: applications
  and tools for calibration and validation", *Cancer Biology & Therapy* 25(1):2344600 (2024).
- C. Bunne et al., "How to Build the Virtual Cell with Artificial Intelligence: Priorities and
  Opportunities", arXiv:2409.11654 (2024).
- R. A. Richardson et al., "EasyVVUQ: A Library for Verification, Validation and Uncertainty Quantification
  in High Performance Computing", *Journal of Open Research Software* 8(1) (2020).
- M. Viceconti, M. De Vos, S. Mellone, L. Geris, "From the Digital Twins in Healthcare to the Virtual Human
  Twin", *IEEE JBHI* 28(1):491–501 (2024).

**Not read, and to be verified before anyone relies on them.** The claims in this note about failure modes
and about methods come from general knowledge rather than from these papers in hand, and the field moves
quickly enough that even that knowledge may be out of date. The areas, with the works I believe are the
standard entry points: neural operators for partial differential equations (Fourier Neural Operator; DeepONet;
physics-informed neural networks); the documented reporting bias from weak baselines in that literature
(McGreivy and Hakim, around 2024); simulation-based inference (Cranmer, Brehmer and Louppe, PNAS, around
2020); digital twins as an engineering discipline (Niederer, Sacks, Girolami and Willcox, around 2021;
Kapteyn, Pretorius and Willcox on predictive twins); closed-loop stimulation of neuronal cultures on
high-density arrays (Kagan et al., *Neuron*, around 2022) and the organoid-intelligence programme that
followed; community benchmarks for generative surrogates in physics (the CaloChallenge); and differentiable
agent-based models (work on gradient-based calibration in epidemiology).
