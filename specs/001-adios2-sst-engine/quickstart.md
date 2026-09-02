# Quickstart Validation Guide: ADIOS2 SST Engine Support

**Branch**: `halehawk/adios2-sst-engine` | **Date**: 2026-09-01

This guide describes how to validate that the SST engine feature works end-to-end once
implementation is complete. It covers build setup, the paired writer/reader smoke test,
parameter tuning verification, and negative-path checks. Implementation code (test bodies,
CMake wiring) belongs in tasks.md and the implementation phase.

---

## Prerequisites

1. **ADIOS2 ≥ 2.9.0 built with SST support** — verify with:
   ```
   adios2_config --has-SST
   ```
   Expected output: `YES`. If `NO`, SST is unavailable at runtime (see Negative Test 3).

2. **SCORPIO configured with `WITH_ADIOS2=ON`**:
   ```
   CC=mpicc CXX=mpicxx FC=mpif90 cmake -DWITH_ADIOS2=ON -DADIOS2_DIR=<adios2-install>/lib/cmake/adios2 <scorpio-src>
   ```

3. **MPI runtime available** (OpenMPI, MPICH, or equivalent).

4. **Build SCORPIO and tests**:
   ```
   make && make tests
   ```

---

## Validation Scenario 1 — SST Writer/Reader End-to-End (FR-007, SC-002, SC-005)

**What it proves**: A SCORPIO writer configured for `PIO_IOTYPE_ADIOS_SST` successfully
streams N timesteps of distributed array data to a concurrently running reader, with no
file written to disk.

**Setup**: No pre-existing stream files needed; SST rendezvous is in-memory.

**Run command** (CTest handles MPI invocation automatically):
```
ctest -R sst_writer_reader -V
```

**Expected outcome**:
- Test exits with status 0.
- Reader verifies that each received variable's values match what the writer wrote for
  every timestep.
- No `.bp` or `.sst` files appear in the test output directory.
- CTest output shows pass for all MPI rank counts in the test matrix.

**Manual run** (for debugging outside CTest):
```
mpirun -np 4 ./sst_writer_test  <stream-name> &
mpirun -np 4 ./sst_reader_test  <stream-name>
```
Replace `<stream-name>` with any short string (e.g., `"scorpio_sst_test"`). Both
processes must use the same stream name.

---

## Validation Scenario 2 — No Disk Artifact (FR-002, SC-002)

**What it proves**: SST mode writes no intermediate file to disk.

**Check**:
```
ls -la <test-output-dir>/   # before test
ctest -R sst_writer_reader
ls -la <test-output-dir>/   # after test
```

**Expected outcome**: No new `.bp`, `.bp.dir`, or `.sst` entries appear in the output
directory between the two `ls` runs.

---

## Validation Scenario 3 — BP-Switching Backward Compatibility (SC-001)

**What it proves**: An existing test that uses `PIO_IOTYPE_ADIOS` (BP5) still passes
unchanged after adding SST support. The engine selection change did not regress BP5.

**Run command**:
```
ctest -R ncdf_eh_one_test_intrnl_ad -V
```
Or run the full existing ADIOS test suite:
```
ctest -R adios -V
```

**Expected outcome**: All existing ADIOS CTest cases continue to pass with the same
pass/fail status as before the SST changes.

---

## Validation Scenario 4 — SST Parameter Configuration (FR-004, SC-004)

**What it proves**: Setting a non-default SST parameter value through SCORPIO's
configuration mechanism takes effect at the ADIOS2 layer.

**Setup**: Configure the writer test to set `QueueLimit=2` and `QueueFullPolicy=Block`.
Run the writer with a deliberately slow reader to fill the queue and observe that the
writer blocks (does not drop data).

**Indicator of success**: Writer's `begin_step` call for timestep 3 (when queue is full
at limit 2) does not return until the reader has consumed at least one queued step.

---

## Validation Scenario 5 — Rendezvous Timeout Error (FR-005, SC-003)

**What it proves**: When no reader attaches, the writer surfaces a specific, actionable
error within the configured timeout window rather than hanging.

**Run command** (writer only, no reader launched):
```
mpirun -np 4 ./sst_writer_test <stream-name>
```

**Expected outcome**:
- The writer does not hang indefinitely.
- SCORPIO returns a non-zero error code.
- The error message contains the stream name and identifies the failure as a rendezvous
  timeout (not a generic I/O error).
- The process exits within the ADIOS2 SST rendezvous timeout window (default: ADIOS2
  upstream default, typically several seconds; configurable via ADIOS2 SST parameters).

---

## Validation Scenario 6 — Build Without ADIOS2 SST (FR-006)

**What it proves**: Using `PIO_IOTYPE_ADIOS_SST` on a build where ADIOS2 lacks SST
transport support produces a clear error, not a crash or silent fallback.

**Setup**: Use an ADIOS2 build without SST (`adios2_config --has-SST` returns `NO`).
Configure and build SCORPIO with `WITH_ADIOS2=ON` against this build.

**Run command**:
```
mpirun -np 4 ./sst_writer_test <stream-name>
```

**Expected outcome**:
- SCORPIO returns `PIO_EADIOS2ERR` (not a segfault or silent success with a fallback engine).
- The error message explicitly identifies SST as the engine that could not be initialized
  and includes the stream name.

---

## Validation Scenario 7 — Fortran Interface (FR-009, SC-006)

**What it proves**: The Fortran constant `PIO_iotype_adios_sst` is accessible and
functional.

**Run command**:
```
ctest -R sst_fortran -V
```

**Expected outcome**: The Fortran SST writer/reader test passes with the same semantics
as the C test in Scenario 1.

---

## Artifact References

- See [contracts/api-changes.md](contracts/api-changes.md) for the exact enum values,
  Fortran constants, and parameter schema.
- See [data-model.md](data-model.md) for stream lifecycle state transitions and entity
  definitions.
- See [research.md](research.md) for engine selection decisions and SST parameter
  defaults.
