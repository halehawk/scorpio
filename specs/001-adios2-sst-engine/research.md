# Research: ADIOS2 SST Engine Support

**Branch**: `halehawk/adios2-sst-engine` | **Date**: 2026-09-01

## Decision 1: New I/O Type Enum Value

**Decision**: Add `PIO_IOTYPE_ADIOS_SST = 10` to `enum PIO_IOTYPE` in `src/clib/pio.h`.

**Rationale**: The existing enum assigns sequential integer values (1–9). ADIOS SST is a
distinct I/O modality (streaming, no persistent file) from ADIOS BP (file-based). A
separate enum value is consistent with how `PIO_IOTYPE_ADIOSC` (compression variant,
value 7) was added alongside `PIO_IOTYPE_ADIOS` (value 6). This lets iotype-dispatch
`switch` statements in `pioc_support.cpp` route SST opens to a different code path than
BP5 opens.

**Alternatives considered**:
- Reuse `PIO_IOTYPE_ADIOS` with an extra flag — rejected; would require API changes to
  file-open calls and break existing callers that dispatch on iotype.
- Add a separate `PIO_IOTYPE_ADIOS_SST_READ` — rejected; unneeded complexity; ADIOS2 SST
  distinguishes read vs. write through the open mode (`adios2_mode_write` /
  `adios2_mode_read`) already, not through the engine.

---

## Decision 2: Engine Selection Implementation Strategy

**Decision**: In `pioc_support.cpp`, replace the two hardcoded `adios2_set_engine(...,
"BP5")` calls (write path ~line 3128, read path ~line 4714) with a conditional that maps
iotype to engine string:

```
PIO_IOTYPE_ADIOS / PIO_IOTYPE_ADIOSC → "BP5"
PIO_IOTYPE_ADIOS_SST                 → "SST"
```

Each guarded block also sets engine-appropriate parameters via `adios2_set_parameter`.
BP-specific parameters (`CollectiveMetadata`, `StatsLevel`, `BufferVType`) are not set
for SST; SST-specific parameters (`DataTransport`, `MarshalMethod`, `RendezvousReaderCount`,
`QueueFullPolicy`, `QueueLimit`) are set only for SST.

**Rationale**: Minimal-invasive change; reuses all existing `adios2_open`, `begin_step`,
`end_step`, `get`, `put` plumbing. ADIOS2 SST engine is wire-compatible with BP at the
ADIOS2 API level — only engine selection and parameter tuning differ.

**Alternatives considered**:
- Separate write/read function pairs for SST — rejected; the ADIOS2 API is engine-agnostic
  above the engine-selection layer; duplicating call chains would violate DRY and make
  the common bug-fix surface larger.

---

## Decision 3: SST Build-Time Availability Guard

**Decision**: No new CMake flag. SST transport availability is determined by the ADIOS2
build itself — if the ADIOS2 installation was built without SST's transport dependencies
(EVPATH, MPI rendezvous), `adios2_set_engine(..., "SST")` followed by `adios2_open` will
return an ADIOS2 error. SCORPIO MUST catch that error and convert it to a clear
`PIO_EADIOS2ERR` with a message identifying SST as the unavailable engine (FR-006).

**Rationale**: SCORPIO already gate-checks ADIOS2 availability at the `WITH_ADIOS2` CMake
level. Requiring users to also set `WITH_ADIOS2_SST=ON` when SST is a sub-feature of the
already-found ADIOS2 adds friction without benefit. Runtime detection produces a clearer
error at the point of actual use.

**Alternatives considered**:
- Add a `WITH_ADIOS2_SST` CMake flag with a compile-time guard — rejected; ADIOS2 does
  not expose a preprocessor macro for SST availability; the check would require a
  CMake try-compile probe, adding build complexity for marginal benefit.

---

## Decision 4: SST Configuration Parameter Mechanism

**Decision**: SST engine parameters (`DataTransport`, `MarshalMethod`, `RendezvousReaderCount`,
`QueueFullPolicy`, `QueueLimit`) are passed via `adios2_set_parameter` in the SST engine
setup block. Default values match ADIOS2 SST upstream defaults. User-facing configuration
is exposed through SCORPIO's existing `PIOc_set_iosystem_error_handling` / rearranger
parameter pathway: SST-specific parameters can be packed as key=value pairs in the
engine's configuration string (same mechanism as `file_desc_t.transport` and
`file_desc_t.params` fields already present in the structure).

For v1, four parameters are supported at minimum (per FR-004):
| Parameter | ADIOS2 SST Key | SCORPIO config key |
|-----------|---------------|-------------------|
| Data transport | `DataTransport` | `sst_data_transport` |
| Marshaling method | `MarshalMethod` | `sst_marshal_method` |
| Queue limit (steps) | `QueueLimit` | `sst_queue_limit` |
| Rendezvous timeout (sec) | `RendezvousReaderCount`+timeout | `sst_rendezvous_timeout` |

**Rationale**: Aligns with the spec requirement (FR-004) for parameter configurability
without source changes. Uses existing struct fields rather than new API surface.

**Alternatives considered**:
- Expose parameters via new `PIOc_set_sst_parameter()` API — deferred to v2; adds public
  API surface that then requires Fortran parity; the existing key=value mechanism
  achieves the same result with zero new API.

---

## Decision 5: SST Writer/Reader Test Architecture

**Decision**: Implement a paired MPI writer/reader test where writer and reader processes
are launched as separate MPI tasks within the same `mpirun` invocation, communicating
through a shared SST stream name. The test is registered via `add_pio_test` in
`tests/general/CMakeLists.txt` with `MINNUMPROCS ≥ 2` to ensure parallel execution.

Test structure:
- Writer ranks write N timesteps of distributed array data, then close the stream.
- Reader ranks open the stream, read N timesteps, compare against writer's values, then
  exit cleanly.
- Both writer and reader are independent executables launched together via CTest's
  multi-executable support, or a single binary that splits behavior by rank range.

**Rationale**: Satisfies FR-007 and Constitution Principle IV (parallel-only testing).
The paired-executable pattern is consistent with how ADIOS2's own SST example tests
are structured. CTest handles the MPI launcher transparently.

**Alternatives considered**:
- Fork a child process from within a single test binary — rejected; non-portable across
  HPC MPI environments; violates CTest/MPI launcher idiom.
- Use a file-system rendezvous point — rejected; SST handles rendezvous internally via
  its named stream; no file system rendezvous needed.

---

## Decision 6: Fortran Interface Parity

**Decision**: Add `PIO_iotype_adios_sst = 10` as a named integer parameter to
`src/flib/pio_types.F90` inside the existing `#ifdef _ADIOS2` block. Add the string
mapping `"ADIOS_SST"` to `PIO_TF_Iotype_from_str` in `tests/general/util/pio_tutil.F90`.
Add at least one Fortran-language variant of the SST write/read test (per FR-009, SC-006).

**Rationale**: Required by Constitution Principle III (C and Fortran Interface Parity).
The pattern mirrors how `PIO_iotype_adiosc = 7` was added alongside `PIO_iotype_adios = 6`.

**Alternatives considered**:
- Fortran parity deferred to v2 — rejected; Constitution Principle III is non-negotiable
  per the project governance document.

---

## Resolved Clarifications

All spec NEEDS CLARIFICATION markers were resolved before this research phase. No open
questions remain. Key scope constraints confirmed:
- Single concurrent reader per stream for v1
- Checkpoint/restart from SST stream: out of scope
- SST availability on platforms where ADIOS2 was built without SST: runtime error, not
  compile-time gate
