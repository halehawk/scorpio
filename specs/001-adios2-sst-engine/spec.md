# Feature Specification: ADIOS2 SST Engine Support

**Feature Branch**: `001-adios2-sst-engine`

**Created**: 2026-09-01

**Status**: Draft

**Input**: User description: "add ADIOS2 SST engine and its related feature"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stream model output to a live consumer without touching disk (Priority: P1)

A model developer configures their SCORPIO-based simulation to write output using the
ADIOS2 SST (Sustainable Staging Transport) engine instead of the existing ADIOS2 BP-file
engine. While the simulation runs, a separate, concurrently-running analysis or
visualization application attaches to the same stream and receives each timestep's data
as it is produced, with no intermediate file ever written to persistent disk.

**Why this priority**: This is the core value of SST support — in-situ/staging coupling
between a running model and a companion consumer — and is the minimum capability needed
for the feature to be usable at all.

**Independent Test**: Start a writer application configured for the SST engine and,
before the writer's rendezvous timeout expires, start a companion reader application
requesting the same stream name. Verify the reader receives the written variables and
decomposition data for each timestep, and that no output file appears on disk for that
stream.

**Acceptance Scenarios**:

1. **Given** a SCORPIO-based writer configured to use the ADIOS2 SST engine, **When** the
   writer opens the output stream and a companion reader attaches within the rendezvous
   window, **Then** the reader receives the writer's variable and decomposition metadata
   and each subsequently written timestep's data.
2. **Given** an SST stream with a single attached reader, **When** the writer closes the
   stream after its final timestep, **Then** the reader observes end-of-stream and exits
   cleanly without error.

---

### User Story 2 - Read a live SST stream through SCORPIO's existing read API (Priority: P1)

A model developer builds a SCORPIO-based consumer application that reads a stream
produced by an SST writer using the same public read API (e.g. `PIOc_read_darray` and
related calls) that SCORPIO already provides for the ADIOS2 BP-file engine, so that
existing SCORPIO-based analysis code can be pointed at a live stream with minimal changes.

**Why this priority**: Without a matching consumer path inside SCORPIO itself, the
producer-side capability from User Story 1 is only usable by hand-written ADIOS2 client
code, not by SCORPIO's own ecosystem of tools and coupled model components.

**Independent Test**: Point a SCORPIO-based reader configured for the SST engine at a
running SST writer's stream name and verify it can decompose and read back variable data
matching what the writer produced for a given timestep.

**Acceptance Scenarios**:

1. **Given** a running SST writer, **When** a SCORPIO-based reader opens the same stream
   name using the SST engine, **Then** the reader can query variable and dimension
   metadata and read distributed array data for each timestep the writer produces.
2. **Given** a SCORPIO-based reader attached to an SST stream, **When** the writer has not
   yet produced a given timestep, **Then** the reader's request for that timestep blocks
   or reports "not yet available" rather than returning incorrect or stale data.

---

### User Story 3 - Tune streaming behavior via configuration (Priority: P2)

A model developer sets SST-specific engine parameters — such as data transport (e.g.
sockets vs. RDMA-capable interconnect), marshaling method, queue depth, and rendezvous
timeout — through SCORPIO's existing I/O-type configuration mechanism, so that streaming
behavior can be tuned per platform without modifying model source code.

**Why this priority**: Reasonable defaults let User Stories 1 and 2 work out of the box,
but production HPC use requires platform-specific tuning (e.g. selecting a
high-performance transport on a supercomputer's interconnect vs. sockets on a
workstation).

**Independent Test**: Set a non-default SST parameter value (e.g. a shorter rendezvous
timeout) through configuration, run the writer/reader pair from User Story 1, and confirm
the configured value takes effect (e.g. rendezvous fails within the shorter window when no
reader attaches).

**Acceptance Scenarios**:

1. **Given** an SST-specific configuration parameter is set to a non-default value,
   **When** the writer opens its stream, **Then** the writer's runtime behavior reflects
   the configured value.
2. **Given** an unsupported or invalid value is set for an SST-specific parameter,
   **When** the writer attempts to open its stream, **Then** SCORPIO reports a clear,
   actionable configuration error rather than failing with an unrelated or generic error.

---

### User Story 4 - Diagnose failed or dropped coupling (Priority: P3)

A model developer sees a clear, actionable error message when SST coupling fails — for
example, no reader attaches before the rendezvous timeout expires, or the reader
disconnects unexpectedly mid-stream — so coupling problems can be diagnosed without
digging through low-level ADIOS2 internals.

**Why this priority**: Streaming coupling introduces failure modes (timing, network,
process lifecycle) that file-based I/O does not have; without clear diagnostics, these
failures are hard to distinguish from ordinary I/O errors.

**Independent Test**: Start a writer configured for the SST engine without ever starting a
companion reader, and verify the writer reports a specific, actionable rendezvous-timeout
error rather than a generic I/O failure or an indefinite hang.

**Acceptance Scenarios**:

1. **Given** a writer configured for the SST engine, **When** no reader attaches before
   the rendezvous timeout expires, **Then** SCORPIO reports an explicit rendezvous-timeout
   error identifying the stream name.
2. **Given** a reader attached to an active SST stream, **When** the writer terminates
   unexpectedly (e.g. crash) before closing the stream, **Then** the reader receives a
   clear disconnect/end-of-stream indication rather than hanging indefinitely.

---

### Edge Cases

- What happens when a writer configured for SST is run without `_ADIOS2` (and its SST
  transport) enabled in the build? System must report a clear "engine not available"
  configuration error at stream-open time, not a crash or silent fallback to file-based
  I/O.
- What happens when a second reader attempts to attach to a stream that already has one
  active reader? Per this feature's scope, a single-reader-only model is assumed;
  behavior for a second concurrent reader is undefined for v1 and MUST be documented as
  unsupported rather than silently accepted.
- How does the system behave if a writer attempts a checkpoint/restart while using the
  SST engine? Streaming output is non-persistent by nature; restart-from-stream is out of
  scope for this feature (see Assumptions).
- What happens if the reader is slower than the writer and falls behind? The engine's
  configured queue limit governs how much backlog is buffered; behavior when the queue is
  exceeded (e.g. writer blocks vs. drops oldest data) must be documented and testable.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: SCORPIO MUST allow a writer application to select the ADIOS2 SST engine as
  an I/O engine option at file/stream-open time, alongside the existing ADIOS2 BP-file
  engine option.
- **FR-002**: SCORPIO MUST allow writer applications to define variables, decomposition
  metadata, and write distributed array data to an SST stream using the same public write
  API already used for the ADIOS2 BP-file engine, requiring no application-level API
  changes for existing callers beyond selecting the SST engine.
- **FR-003**: SCORPIO MUST allow reader applications to open an SST stream and read
  variable metadata and distributed array data using the same public read API already
  used for the ADIOS2 BP-file engine.
- **FR-004**: SCORPIO MUST support configuring SST-specific engine parameters (at minimum:
  data transport, marshaling method, queue limit, and rendezvous timeout) through
  SCORPIO's existing I/O-type/rearranger configuration mechanism, without requiring
  application source changes.
- **FR-005**: SCORPIO MUST report a specific, actionable error when an SST rendezvous does
  not complete within the configured timeout, distinguishable from generic I/O failures.
- **FR-006**: SCORPIO MUST report a specific, actionable error when a writer or reader
  attempts to use the SST engine in a build where SST support is not compiled in, rather
  than failing silently or falling back to a different engine.
- **FR-007**: SCORPIO's automated test suite MUST include at least one paired
  writer/reader test that exercises the SST engine end-to-end (stream open, metadata
  exchange, timestep data write/read, stream close).
- **FR-008**: SCORPIO MUST document, and its behavior MUST match, that only a single
  concurrent reader per SST stream is supported in this feature's scope.

### Key Entities

- **SST Stream**: The named, transient communication channel between one writer and one
  reader, replacing an on-disk file for the duration the two are connected. Exists only
  while writer and reader are both active (or within the rendezvous/queue window).
- **Engine Configuration**: The set of SST-specific tunable parameters (data transport,
  marshaling method, queue limit, rendezvous timeout) associated with a given I/O
  decomposition/file handle.
- **Rendezvous**: The handshake period during which a writer waits for a reader (or vice
  versa) to attach to a named stream before proceeding or timing out.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A model developer can switch an existing ADIOS2-BP-based SCORPIO writer to
  the SST engine by changing configuration alone, with zero changes to application
  read/write calls.
- **SC-002**: A companion reader receives each written timestep's data within the
  engine's configured queue/latency bounds, with no intermediate file created on disk.
- **SC-003**: When no reader attaches, the writer surfaces a rendezvous-timeout error
  within the configured timeout window, rather than hanging indefinitely.
- **SC-004**: 100% of the SST-specific configuration parameters listed in FR-004 are
  settable without modifying application source code.
- **SC-005**: The paired writer/reader SST test (FR-007) passes reliably in CI/nightly
  testing across supported platforms.

## Assumptions

- Both producer (write) and consumer (read) sides are in scope, mirroring the existing
  ADIOS2 BP-file engine support already present in SCORPIO.
- Only a single concurrent reader per SST stream is in scope for this feature;
  multi-reader fan-out is explicitly out of scope and may be considered as a future
  enhancement.
- Checkpoint/restart from an SST stream is out of scope, since streamed data is not
  persisted; restart continues to rely on file-based engines.
- The underlying ADIOS2 library used by SCORPIO is built with SST engine support
  (`_ADIOS2` plus SST's transport dependencies) available on target platforms; this
  feature does not add SST support to platforms where the ADIOS2 dependency itself lacks
  it.
- Default values for SST-specific parameters (transport, marshaling method, queue limit,
  rendezvous timeout) follow ADIOS2's own upstream SST defaults unless a SCORPIO-specific
  override is explicitly configured.
