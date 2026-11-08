# Contributing

## Workflow

1. Pick or open an issue; every issue belongs to a milestone of `docs/ROADMAP.md`.
2. Work on a branch and open a pull request; fill in the template checklist.
3. The **CI passed** check must be green before merging. Maintainers add the `run-benchmarks`
   label to performance-sensitive pull requests once the benchmark machine exists.
4. Sign off every commit (`git commit -s`) to certify the
   [Developer Certificate of Origin](https://developercertificate.org/).

## Local checks

```
cmake -S . -B build -G Ninja -DABMIO_WARNINGS_AS_ERRORS=ON && cmake --build build && ctest --test-dir build
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DABMIO_SANITIZE=address,undefined && cmake --build build-asan && ctest --test-dir build-asan
scripts/check-policy.sh
pipx install clang-format==23.1.3   # the version CI uses
```

The HDF5 tests (label `container`) need `libhdf5-dev` or conda-forge's `hdf5`; with HighFive installed,
`-DABMIO_HIGHFIVE=ON` builds and tests both sides of the binding comparison, and
`bench/h5binding/compare.py build` runs it with `h5diff`, `h5dump`, `cmp` and `h5py`.

Fuzzing needs Clang:

```
cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DABMIO_BUILD_FUZZERS=ON -DABMIO_SANITIZE=address,undefined -DABMIO_BUILD_TESTS=OFF
cmake --build build-fuzz && build-fuzz/fuzz_unpack -max_total_time=60 fuzz/corpus/fuzz_unpack
```

## Rules that CI enforces

- No global `operator new`/`delete` replacement.
- Every source file starts with `// SPDX-License-Identifier: Apache-2.0`.
- Code is formatted with the repository's `.clang-format` (BioDynaMo's style).
- Line coverage of the library stays at or above 85%.
