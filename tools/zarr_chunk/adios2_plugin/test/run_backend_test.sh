#!/bin/bash
# Run zarr_backend_test once with the Zarr (plugin) backend and once with
# BP5, then compare the two outputs.
#   run_backend_test.sh <workdir> <writer> <xml dir> <python> <compare.py> [launcher ...]
# e.g. launcher "mpiexec -n 3" for the MPI build.
set -e
workdir=$1 writer=$2 xmldir=$3 python=$4 compare=$5
shift 5
mkdir -p "$workdir" && cd "$workdir"
rm -rf backend_test.zarr backend_test.bp
"$@" "$writer" "$xmldir/zarr_backend.xml" backend_test.zarr
"$@" "$writer" "$xmldir/bp5_backend.xml" backend_test.bp
"$python" "$compare" backend_test.bp backend_test.zarr
