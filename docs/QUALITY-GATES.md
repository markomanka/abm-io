# Test environment and quality gates

Almost every check runs on GitHub-hosted runners. Two need machines you provide: a dedicated benchmark
server, because shared runners are too noisy to catch 5–10% slowdowns, and an HPC allocation, because
hosted runners have neither several nodes nor real interconnects. Both workflows stay idle until you set
the repository variables below.

## Checks

| Layer | Workflow · job | Runs on | When | Guards |
| --- | --- | --- | --- | --- |
| Policies (no global allocator hooks, SPDX headers) and formatting | `ci.yml` · Policy and format | GitHub-hosted | every push and PR | all |
| Build matrix: Ubuntu 22.04 and 24.04 × GCC and Clang; macOS 15 | `ci.yml` · Build and test | GitHub-hosted | every push and PR | all |
| Build and tests with conda-forge packages (`environment.yml`): Ubuntu 24.04 and macOS 15 | `conda.yml` | GitHub-hosted | every push and PR | packaging |
| Unit, corruption, codec and physics tests (CTest) | `ci.yml` · Build and test | GitHub-hosted | every push and PR | all |
| AddressSanitizer + UBSan | `ci.yml` · Sanitizers | GitHub-hosted | every push and PR | all |
| MPI correctness with MPICH and Open MPI, 2 ranks | `ci.yml` · MPI | GitHub-hosted | every push and PR | exchange |
| Line coverage ≥ 85% | `ci.yml` · Coverage | GitHub-hosted | every push and PR | all |
| Fuzzing under address and undefined sanitizers | `cflite-pr.yml` (5 min), `cflite-batch.yml` (1 h) | GitHub-hosted | PRs touching code; nightly | security |
| CodeQL, security-extended queries | `codeql.yml` | GitHub-hosted | push, PR, weekly | releases |
| OSV-Scanner | `osv-scanner.yml` | GitHub-hosted | push, PR, weekly | releases |
| OpenSSF Scorecard | `scorecard.yml` | GitHub-hosted | weekly; push to `main` | releases |
| Benchmarks against a stored baseline (±5%) | `benchmarks.yml` | your benchmark machine | nightly; PRs labelled `run-benchmarks` | performance |
| Multi-node MPI | `hpc.yml` | your HPC allocation | weekly; manual | exchange at scale |

`CI passed` aggregates all `ci.yml` jobs; branch protection requires it. `conda.yml` is not part of it yet:
it can join once it has run reliably, so that a shortage of macOS runners cannot block merges. All third-party actions are
pinned to commit hashes of releases at least two weeks old; Dependabot proposes updates weekly.

## Repository variables

| Variable | Set by | Effect |
| --- | --- | --- |
| `PUBLIC_REPO` | `scripts/bootstrap-github.sh` | Enables CodeQL, Scorecard and OSV-Scanner, which are free only on public repositories |
| `HAS_BENCHMARK_RUNNER` | you, after registering the machine | Enables `benchmarks.yml` |
| `HAS_HPC_RUNNER` | you, after preparing the site | Enables `hpc.yml` |

## Adding the benchmark machine

1. Bare metal with two sockets and 32 or more cores. Fix the CPU frequency, turn off turbo and SMT,
   and run nothing else on it.
2. Register a self-hosted runner (Settings → Actions → Runners → New self-hosted runner) with the
   label `benchmark`. Prefer ephemeral mode (`./config.sh --ephemeral`) under a service that re-registers.
3. Install CMake, Ninja, pkg-config, LZ4, GoogleTest and Python 3.
4. `gh variable set HAS_BENCHMARK_RUNNER --body true`
5. Run Actions → Benchmarks → Run workflow with `record = true`, download the artifact and commit
   `bench/baseline/<host>.json`. That completes milestone 0.

## Adding the HPC allocation

- Many centres forbid persistent runners on login nodes. Use a runner (label `hpc`) on a CI node that can
  submit Slurm jobs, or the centre's own CI integration (for example Jacamar CI for GitLab) mirroring
  this repository.
- Adapt `scripts/hpc/multinode.sbatch` to the site's modules and partitions, then
  `gh variable set HAS_HPC_RUNNER --body true`.

## Safety rules for self-hosted runners

- Never run code from forks. `benchmarks.yml` runs only on schedules, manual dispatch, or pull requests
  from branches of this repository that carry the `run-benchmarks` label.
- Keep Settings → Actions → General → "Require approval for all outside collaborators" enabled.
- Use ephemeral runners, an isolated network and no credentials beyond what the job needs.
