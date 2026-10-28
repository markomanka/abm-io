#!/bin/bash
# Template for the weekly multi-node run (.github/workflows/hpc.yml). Adapt to the site first.
#SBATCH --job-name=abmio-multinode
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --time=00:30:00
#SBATCH --output=hpc-%j.log
set -euo pipefail
# Site-specific environment, for example:
#   module load gcc openmpi
srun build-hpc/abmio_mpi_test
