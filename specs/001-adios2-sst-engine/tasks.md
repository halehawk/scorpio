# Tasks: ADIOS2 SST Engine Support

**Input**: Design documents from `specs/001-adios2-sst-engine/`

**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/api-changes.md ✅, quickstart.md ✅

**Tests**: Included per FR-007 (C paired test) and FR-009 / SC-006 (Fortran test).

**Organization**: Tasks grouped by user story. US1 and US2 are both P1 but ordered:
writer-path (US1) first since the reader test depends on a working writer.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel with other [P] tasks in the same phase (different files, no shared state)
- **[Story]**: Which user story this task belongs to (US1–US4)

---

## Phase 1: Setup

**Purpose**: Create the new test directory structure and verify SST availability.

- [x] T001 Create `tests/general/sst/` directory with a stub `CMakeLists.txt` that includes the subdirectory from `tests/general/CMakeLists.txt`
- [x] T002 [P] Add `include(tests/general/sst/CMakeLists.txt)` (or `add_subdirectory`) in `tests/general/CMakeLists.txt` under the existing `#ifdef _ADIOS2`-equivalent CMake block

**Checkpoint**: `cmake` completes without error; `tests/general/sst/` exists in the build tree.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Expose the new `PIO_IOTYPE_ADIOS_SST` constant in the C and Fortran public
interfaces and wire up the string-conversion helpers. All user story phases depend on
these changes being complete first.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [x] T003 Add `PIO_IOTYPE_ADIOS_SST = 10` to `enum PIO_IOTYPE` in `src/clib/pio.h` inside the existing `#ifdef _ADIOS2` block, after `PIO_IOTYPE_HDF5C = 9`
- [x] T004 [P] Add `PIO_iotype_adios_sst = 10` integer parameter to `src/flib/pio_types.F90` inside the existing `#ifdef _ADIOS2` block, alongside `PIO_iotype_adios = 6` and `PIO_iotype_adiosc = 7`
- [x] T005 [P] Add `case PIO_IOTYPE_ADIOS_SST: return "PIO_IOTYPE_ADIOS_SST";` to `pio_iotype_to_string()` in `src/clib/util/pio_print.cpp`
- [x] T006 [P] Add `"ADIOS_SST"` / `"PIO_IOTYPE_ADIOS_SST"` / `"pio_iotype_adios_sst"` string mappings to `PIO_TF_Iotype_from_str` in `tests/general/util/pio_tutil.F90` within the `#ifdef _ADIOS2` block
- [x] T007 [P] Add `PIO_IOTYPE_ADIOS_SST` to the iotype array returned by `PIO_TF_Get_iotypes()` in `tests/general/util/pio_tutil.F90`

**Checkpoint**: `make` compiles without error; `PIO_IOTYPE_ADIOS_SST` is a valid C constant and `PIO_iotype_adios_sst` is a valid Fortran parameter. No runtime behavior change yet.

---

## Phase 3: User Story 1 — Write Path (Priority: P1) 🎯 MVP Start

**Goal**: A SCORPIO writer application can open an SST stream, write distributed array
data for multiple timesteps, and close the stream cleanly — with no intermediate file
written to disk. Existing `PIO_IOTYPE_ADIOS` (BP5) behavior is unchanged.

**Independent Test**: Run the C writer test with `RendezvousReaderCount=0` (no reader
needed); verify the test exits 0, `adios2_open` returns success, N timestep
`begin_step`/`end_step` cycles complete, and no `.bp` or `.sst` artifact appears in the
working directory.

### Implementation for User Story 1

- [x] T008 [US1] In `src/clib/core/pioc_support.cpp`, extend the file-creation iotype dispatch condition (~line 3115) to include `PIO_IOTYPE_ADIOS_SST`
- [x] T009 [US1] In `pioc_support.cpp` write-path engine-setup block (~line 3199), conditional engine string: SST iotype → `"SST"`; ADIOS/ADIOSC → `"BP5"`
- [x] T010 [US1] In `initialize_adios2_variables()`, skip BP5-only parameters for SST; add `RendezvousReaderCount=1` and SST transport defaults; support `file->transport` and `file->params` pass-through
- [x] T011 [US1] After `adios2_open` for the SST write path in `pioc_support.cpp` (~line 3242), error handling includes stream name (covers FR-006 write side)
- [x] T012 [US1] Created `tests/general/sst/pio_sst_writer.c`: standalone C writer program; all ranks use `PIOc_createfile` with `PIO_IOTYPE_ADIOS_SST`, writes 3 timesteps via `PIOc_write_darray`
- [x] T013 [US1] CMake target `pio_sst_writer` in `tests/general/sst/CMakeLists.txt`, guarded by `if(WITH_ADIOS2)`; `run_sst_c_paired.sh.in` configured to launch writer+reader via MPMD mpirun
- [x] T014 [US1] CTest `sst_endtoend` registered via `run_sst_c_paired.sh` shell script (2-rank writer + 2-rank reader in one mpirun)

**Checkpoint**: `ctest -R sst_writer_standalone` passes. Writer opens and closes an SST stream with no reader attached (`RendezvousReaderCount=0`). No file artifact on disk.

---

## Phase 4: User Story 2 — Read Path (Priority: P1)

**Goal**: A SCORPIO reader application can open a live SST stream and read distributed
array data per timestep using the same public read API as the BP5 engine, with no
application source changes required beyond specifying `PIO_IOTYPE_ADIOS_SST`.

**Independent Test**: Run the paired C writer/reader test (`sst_writer_reader`); verify
the reader's received values for every timestep match the values written by the writer,
and neither process creates a file on disk. The test runs under CTest with MPI.

### Implementation for User Story 2

- [x] T015 [US2] Added `else if (file->iotype == PIO_IOTYPE_ADIOS_SST)` read path in `pioc_support.cpp` (~line 4829), inside `#ifdef _ADIOS2` block
- [x] T016 [US2] SST read path sets SST engine, `RendezvousReaderCount=1`, opens with `adios2_mode_read` using stream name (no `.bp` lookup)
- [x] T017 [US2] Read-path error handling: `adios2_open` failure returns `PIO_EADIOS2ERR` with stream name and SST identification
- [x] T018 [US2] Created `tests/general/sst/pio_sst_reader.c`: standalone C reader program; all ranks use `PIOc_openfile` with `PIO_IOTYPE_ADIOS_SST`, reads and verifies 3 timesteps via `PIOc_read_darray`
- [x] T019 [US2] CMake target `pio_sst_reader` in `tests/general/sst/CMakeLists.txt`
- [x] T020 [US2] `sst_endtoend` CTest runs `run_sst_c_paired.sh` which launches both executables via MPMD mpirun
- [x] T021 [P] [US2] Create `tests/general/sst/pio_sst_writer_f.F90` and `pio_sst_reader_f.F90`: separate Fortran writer and reader programs using `PIO_iotype_adios_sst`
- [x] T022 [P] [US2] Create `tests/general/sst/run_sst_f_paired.sh.in`: MPMD launch script template configured by CMake to run writer+reader in one mpirun
- [x] T023 [US2] Add CMake targets `pio_sst_writer_f` and `pio_sst_reader_f` in `tests/general/sst/CMakeLists.txt`; guard with `if(WITH_ADIOS2)` and Fortran enabled; configure launch script with configure_file
- [x] T024 [US2] Register Fortran CTest `sst_endtoend_f` in `tests/general/sst/CMakeLists.txt`

**Checkpoint**: `ctest -R sst_writer_reader` passes (C test). `ctest -R sst_writer_reader_f` passes (Fortran test). Reader values match writer values for all 3 timesteps. No disk artifacts.

---

## Phase 5: User Story 3 — SST Parameter Configuration (Priority: P2)

**Goal**: SST-specific engine parameters (DataTransport, MarshalMethod, QueueLimit,
QueueFullPolicy) can be set without modifying application source code, via SCORPIO's
existing I/O system parameter mechanism (the `transport` and `params` fields in
`file_desc_t`).

**Independent Test**: Run the parameterized writer test with `QueueLimit=2` and a
deliberately slow reader; confirm the writer's `begin_step` call for timestep 3 blocks
until the reader has consumed a step, rather than discarding or erroring immediately.

### Implementation for User Story 3

- [x] T025 [US3] `initialize_adios2_variables()` SST branch reads `file->transport` and calls `adios2_set_parameter(file->ioH, "DataTransport", file->transport)` when non-empty
- [x] T026 [US3] Same SST branch parses `file->params` (semicolon-separated `key=value`) via `strtok_r` and calls `adios2_set_parameter` for each pair; error on invalid key/value
- [x] T027 [US3] Error interception for invalid SST params surfaces `PIO_EADIOS2ERR` with param name, value, and stream name
- [ ] T028 [US3] Add a parameterized CTest variant `sst_writer_reader_queued` in `tests/general/CMakeLists.txt`: same paired test as T020 but pass `QueueLimit=2` via the params mechanism; validate via test assertion that writer blocks (does not discard) when queue is full

**Checkpoint**: `ctest -R sst_writer_reader_queued` passes. SC-004: all four parameter keys (DataTransport, MarshalMethod, QueueLimit, QueueFullPolicy) are passable without source changes.

---

## Phase 6: User Story 4 — Failure Diagnostics (Priority: P3)

**Goal**: When SST coupling fails — no reader attaches before timeout, or the writer
crashes mid-stream — SCORPIO surfaces a clear, actionable error identifying the stream
name, rather than hanging indefinitely or emitting a generic I/O error.

**Independent Test**: Run the writer-only test without setting `RendezvousReaderCount=0`
(so the writer blocks for a reader). Use a short ADIOS2 SST rendezvous timeout (set via
`file->params`). Confirm the writer returns within the timeout window with a non-zero
error code whose message includes the stream name and the text "rendezvous" or "SST".

### Implementation for User Story 4

- [x] T029 [US4] In the SST `adios2_open` error path in `pioc_support.cpp` (write side), SST-specific branch returns error including "rendezvous timeout" and stream name when `adios2_open` returns NULL for an SST engine (FR-005)
- [x] T030 [P] [US4] In the SST `adios2_open` error path in `pioc_support.cpp` (read side), error message already includes "rendezvous timeout" and stream name in the `engineH == NULL` check
- [ ] T031 [US4] In the SST read path's `begin_step` call in `pioc_support.cpp`, handle the case where `adios2_begin_step` returns `adios2_step_status_end_of_stream` (writer disconnected): log a clear message and return `PIO_EIO` (or `PIO_EADIOS2ERR`) rather than silently blocking or returning incorrect data (FR-004 / US4 acceptance scenario 2) — requires full SST darray read path implementation
- [ ] T032 [US4] Register a rendezvous-timeout CTest `sst_timeout_error` in `tests/general/CMakeLists.txt` using `add_pio_test` with `WILLFAIL` status; run writer with default `RendezvousReaderCount=1` and a short timeout via params, no reader launched; assert non-zero exit code and error message containing stream name — depends on T031

**Checkpoint**: `ctest -R sst_timeout_error` completes within the configured timeout window and the writer process exits non-zero with a message containing the stream name and "rendezvous".

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, documentation, and regression checks across all stories.

- [ ] T033 [P] Run the full existing ADIOS CTest suite (`ctest -R adios`) and confirm all previously-passing BP5 tests still pass — no regression from the engine-selection branching introduced in T009/T016
- [x] T034 [P] Updated `AGENTS.md` with `PIO_IOTYPE_ADIOS_SST` iotype table, SST engine section, configurable parameters table, constraints, and test reference
- [ ] T035 Run `quickstart.md` Validation Scenario 2 (no disk artifact check) for both the C and Fortran paired tests to confirm no `.bp` / `.sst` files are created in the test output directory
- [ ] T036 [P] Verify C++ formatting of all changed `.cpp` / `.hpp` files against SCORPIO's formatting standard (`https://docs.e3sm.org/scorpio/html/contributing_code.html`) before final commit
- [ ] T037 Run the full CI workflow (`SCORPIO + PnetCDF - build and test`) defined in `.github/workflows/` and confirm it passes with SST tests included

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately.
- **Foundational (Phase 2)**: Depends on Phase 1. **BLOCKS all user story phases.**
- **US1 — Write Path (Phase 3)**: Depends on Phase 2 completion.
- **US2 — Read Path (Phase 4)**: Depends on Phase 3 completion (reader tests require a working writer).
- **US3 — Configuration (Phase 5)**: Depends on Phase 4 (parameter tests use the paired writer/reader from US2).
- **US4 — Diagnostics (Phase 6)**: Depends on Phase 3 (timeout test needs the writer infrastructure).
- **Polish (Phase 7)**: Depends on all prior phases.

### User Story Dependencies

- **US1 (P1)**: Independent after Phase 2. No dependency on US2/US3/US4.
- **US2 (P1)**: Depends on US1 (paired test needs working writer). Can proceed in parallel with US3/US4 once US1 is complete.
- **US3 (P2)**: Depends on US2 (parameter test reuses paired writer/reader). Can proceed in parallel with US4.
- **US4 (P3)**: Depends on US1 writer infrastructure only (T011, T029). Can start after US1 completes.

### Within Each Phase

- Tasks marked [P] within the same phase have no dependencies on each other and can run in parallel.
- T021/T022 (Fortran tests) are parallel with T018–T020 (C tests).
- T009 and T016 (write and read engine branches) are parallel once T008/T015 conditions are landed.

### Parallel Opportunities

```text
Phase 2 (run all in parallel after Phase 1):
  T003, T004, T005, T006, T007

Phase 3 (sequential: T008 → T009/T010/T011 → T012 → T013 → T014):
  T009, T010, T011 can run in parallel after T008

Phase 4 (T015, T016 parallel; T021, T022 parallel with T018):
  T015 ∥ T016 (after T008/T009 done)
  T018 ∥ T021 ∥ T022 (independent files)

Phase 5 (sequential within the params block):
  T025 → T026 → T027 → T028

Phase 6:
  T029 ∥ T030 (after T011/T017); then T031 → T032

Phase 7 (all parallel):
  T033 ∥ T034 ∥ T035 ∥ T036 → T037
```

---

## Parallel Example: User Story 2 (Read Path)

```text
# After US1 is complete, launch these in parallel:

Task T015: Extend read dispatch in pioc_support.cpp
Task T016: Add SST branch to read engine-setup block in pioc_support.cpp

# After T015+T016, launch these in parallel:

Task T018: Create pio_sst_reader_test.c
Task T021: Create pio_sst_writer_test.f90
Task T022: Create pio_sst_reader_test.f90

# Then sequentially:

Task T019: Add CMake target for C reader test
Task T020: Register paired CTest sst_writer_reader
Task T023: Add CMake targets for Fortran tests
Task T024: Register Fortran CTest sst_writer_reader_f
```

---

## Implementation Strategy

### MVP First (User Story 1 — Write Path Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (enum, Fortran constant, string helpers)
3. Complete Phase 3 (US1): Write path — engine selection, params, error handling, writer test
4. **STOP and VALIDATE**: `ctest -R sst_writer_standalone` passes; no disk artifact; existing ADIOS tests still pass
5. Merge or checkpoint before moving to the read path

### Incremental Delivery

1. Setup + Foundational → constants visible in C and Fortran
2. US1 (Write) → writer can open/write/close SST stream ← **MVP**
3. US2 (Read) → paired writer/reader test passes in C and Fortran ← **Full Feature**
4. US3 (Config) → SST parameters configurable without source changes
5. US4 (Diagnostics) → actionable timeout and disconnect errors

### Total Task Count: 37 tasks across 7 phases

| Phase | Story | Tasks | Notes |
|-------|-------|-------|-------|
| 1 Setup | — | T001–T002 | 2 tasks |
| 2 Foundational | — | T003–T007 | 5 tasks, all parallelizable |
| 3 US1 | P1 Write | T008–T014 | 7 tasks |
| 4 US2 | P1 Read | T015–T024 | 10 tasks |
| 5 US3 | P2 Config | T025–T028 | 4 tasks |
| 6 US4 | P3 Diag | T029–T032 | 4 tasks |
| 7 Polish | — | T033–T037 | 5 tasks |

---

## Notes

- [P] tasks = different files, no dependencies on each other within the same phase
- Each user story phase ends with a **Checkpoint** that is independently verifiable
- Existing ADIOS BP5 behavior must not regress — T033 is a blocking gate before merging
- Commit after each Checkpoint at minimum; more granular commits are encouraged
- Do not set `RendezvousReaderCount=0` as a permanent default — it's only for the standalone writer smoke test (T014); the paired test (T020) must use `RendezvousReaderCount=1`
