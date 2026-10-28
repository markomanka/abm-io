## What and why

<!-- Link the issue, e.g. "Closes #12". -->

## Checklist

- [ ] Tests added or updated (unit, corruption, physics or MPI, as relevant)
- [ ] `ctest` passes locally, including a build with `-DABMIO_SANITIZE=address,undefined`
- [ ] Every new decoding path validates counts, offsets, type IDs and indices, and its fuzz harness covers it
- [ ] No global `operator new`/`delete` replacement
- [ ] Performance-sensitive change: a maintainer adds the `run-benchmarks` label
- [ ] `docs/` updated if the API, a file format or the evidence changed
- [ ] Commits signed off (`git commit -s`, Developer Certificate of Origin)
