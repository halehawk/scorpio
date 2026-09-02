# Implementation Plan: ADIOS2 SST Engine Support

**Branch**: `halehawk/adios2-sst-engine` | **Date**: 2026-09-01 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/001-adios2-sst-engine/spec.md`

## Summary

Add ADIOS2 SST (Sustainable Staging Transport) as a new I/O type (`PIO_IOTYPE_ADIOS_SST = 10`)
in SCORPIO, enabling in-situ/staging data coupling between a running simulation (writer)
and a companion consumer (reader) with zero disk I/O. The implementation extends the
existing ADIOS2 BP-engine code path by branching engine selection in `pioc_support.cpp`
and adding the new constant to both the C and Fortran public interfaces. Both a C and a
Fortran paired writer/reader CTest-parallel test are required.

## Technical Context

**Language/Version**: C (C99+), C++ (C++17), Fortran 90/2003; compiled via MPI-enabled
wrappers (`mpicc`, `mpicxx`, `mpif90`)

**Primary Dependencies**:
- ADIOS2 ≥ 2.9.0 (with SST transport built in — requires EVPATH/MPI rendezvous on the
  target platform; sockets transport is universally available)
- MPI (OpenMPI, MPICH, or vendor MPI) — mandatory for all SCORPIO operations
- CMake 2.8.12+ (existing build system)

**Storage**: N/A for SST streams — no persistent files written to disk during normal
SST operation.

**Testing**: CTest with multi-rank MPI execution; `add_pio_test` macro in
`tests/general/CMakeLists.txt`; Fortran test utilities in
`tests/general/util/pio_tutil.F90`

**Target Platform**: HPC Linux (NERSC Perlmutter, NWSC Derecho, ALCF Polaris); also
workstation Linux. SST's socket transport enables testing without RDMA hardware.

**Project Type**: Library (C + Fortran bindings)

**Performance Goals**: SST streaming should eliminate disk I/O round-trips entirely.
Data integrity (correctness) must be verified by comparing reader-received values to
writer-sent values for every timestep. Throughput benchmarking is deferred to post-v1.

**Constraints**:
- MUST NOT break existing `PIO_IOTYPE_ADIOS` or `PIO_IOTYPE_ADIOSC` (BP5) behavior.
- No new CMake option required; SST availability is determined by the ADIOS2 build.
- Single concurrent reader per SST stream in v1.
- Writer and reader MUST be independent MPI programs; SST does not support writer and
  reader in the same MPI communicator.

**Scale/Scope**: Single writer, single concurrent reader; multi-rank MPI on both sides.

## Constitution Check

*GATE: Must pass before implementation. Re-check after Phase 1 design.*

| Principle | Status | Evidence / Mitigation |
|-----------|--------|----------------------|
| I. MPI-First Parallel Correctness | PASS | SST `begin_step`/`end_step` are collective on the writer's MPI communicator — same guarantee as BP5. ADIOS2 SST handles distributed rendezvous across ranks internally. Code review must confirm all SST open/close calls are rank-collective. |
| II. Backend Abstraction and Isolation | PASS | Engine selection (`"SST"` vs. `"BP5"`) is confined to the `iotype` dispatch block in `pioc_support.cpp`. No SST-specific logic leaks into the common API layer or Fortran interface. |
| III. C and Fortran Interface Parity | PASS | `PIO_iotype_adios_sst = 10` added to `src/flib/pio_types.F90`. Fortran writer/reader test required (FR-009, SC-006). |
| IV. Parallel-Only Testing | PASS | SST test registered via `add_pio_test` with `MINNUMPROCS ≥ 2`. CTest handles MPI launcher. No serial fallback. |
| V. Performance and Correctness Over Convenience | PASS | SST eliminates disk I/O — strictly better performance than BP5 for in-situ use cases. Correctness verified by value-comparison in the test. |

**Complexity Tracking**: No constitution violations. No entries required.

## Project Structure

### Documentation (this feature)

```text
specs/001-adios2-sst-engine/
├── plan.md              # This file
├── research.md          # Phase 0 — decisions and alternatives
├── data-model.md        # Phase 1 — entity definitions and state machine
├── quickstart.md        # Phase 1 — end-to-end validation scenarios
├── contracts/
│   └── api-changes.md   # Phase 1 — public API contract changes
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2 output (/speckit-tasks command)
```

### Source Code (repository root)

```text
src/
├── clib/
│   ├── pio.h                         # Add PIO_IOTYPE_ADIOS_SST = 10 to enum
│   ├── core/
│   │   └── pioc_support.cpp          # Branch engine selection; SST params setup
│   └── util/
│       └── pio_print.cpp             # Add string case for PIO_IOTYPE_ADIOS_SST
└── flib/
    └── pio_types.F90                 # Add PIO_iotype_adios_sst = 10 parameter

tests/
└── general/
    ├── CMakeLists.txt                # Register SST CTest targets
    ├── sst/                          # New test directory for SST writer/reader tests
    │   ├── pio_sst_writer_test.c     # C writer test executable
    │   ├── pio_sst_reader_test.c     # C reader test executable
    │   ├── pio_sst_writer_test.f90   # Fortran writer test executable
    │   ├── pio_sst_reader_test.f90   # Fortran reader test executable
    │   └── CMakeLists.txt            # Build targets for SST tests
    └── util/
        └── pio_tutil.F90             # Add ADIOS_SST string mapping
```

**Structure Decision**: SST tests live in a dedicated `tests/general/sst/` subdirectory
to keep them distinct from the existing file-based ADIOS test cases, while remaining
within the same CTest infrastructure.
