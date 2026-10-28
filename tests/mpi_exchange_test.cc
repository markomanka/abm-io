// SPDX-License-Identifier: Apache-2.0
// Every rank packs the same population, sends it to the next rank in a ring,
// and checks what it receives from the previous one. Exit code 77 = skipped.
#include <mpi.h>

#include <climits>
#include <cstdio>
#include <vector>

#include "abmio/exchange.h"
#include "test_util.h"

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  int rank = 0, size = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  if (size < 2) {
    if (rank == 0)
      std::puts("needs at least 2 ranks; skipping");
    MPI_Finalize();
    return 77;
  }
  auto pop = abmio::test::MakePopulation(5000, 2026);
  long long errors = 0;
  for (auto layout : {abmio::Layout::kRows, abmio::Layout::kColumns}) {
    std::vector<char> out;
    const uint64_t len = abmio::Pack(pop.ptrs, layout, out);
    const int to = (rank + 1) % size, from = (rank + size - 1) % size;
    uint64_t in_len = 0;
    MPI_Sendrecv(&len, 1, MPI_UINT64_T, to, 0, &in_len, 1, MPI_UINT64_T, from,
                 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (len > INT_MAX || in_len > INT_MAX)
      MPI_Abort(MPI_COMM_WORLD, 2);
    std::vector<char> in(in_len);
    MPI_Sendrecv(out.data(), static_cast<int>(len), MPI_BYTE, to, 1, in.data(),
                 static_cast<int>(in_len), MPI_BYTE, from, 1, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
    abmio::Arena arena;
    if (abmio::Unpack(in.data(), in_len, arena) != abmio::Status::kOk) {
      ++errors;
    } else {
      errors += static_cast<long long>(
          abmio::test::CountMismatches(pop.ptrs, arena.view));
    }
  }
  long long total = 0;
  MPI_Allreduce(&errors, &total, 1, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0)
    std::printf("MPI exchange across %d ranks: %lld mismatches\n", size, total);
  MPI_Finalize();
  return total == 0 ? 0 : 1;
}
