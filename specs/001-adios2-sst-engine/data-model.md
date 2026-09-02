# Data Model: ADIOS2 SST Engine Support

**Branch**: `halehawk/adios2-sst-engine` | **Date**: 2026-09-01

## Overview

The SST engine does not introduce new persistent storage entities. It reuses SCORPIO's
existing in-memory descriptor structures (`file_desc_t`, `iosystem_desc_t`) and the
ADIOS2 engine/IO handle layer. The data model changes are additive: a new enum value,
a new Fortran constant, and SST-specific parameter fields within existing descriptors.

---

## Entity 1: I/O Type (`PIO_IOTYPE`)

**Location**: `src/clib/pio.h` (C enum), `src/flib/pio_types.F90` (Fortran parameter)

**What it represents**: The identity of the I/O backend used for a file/stream. Every
open file in SCORPIO is tagged with an iotype that routes all subsequent operations
(define variable, write, read, close) to the correct backend implementation.

**Current values** (relevant subset):

| Value | Integer | Meaning |
|-------|---------|---------|
| `PIO_IOTYPE_ADIOS` | 6 | ADIOS2 BP5 file-based engine |
| `PIO_IOTYPE_ADIOSC` | 7 | ADIOS2 BP5 with compression |

**New value added by this feature**:

| Value | Integer | Meaning |
|-------|---------|---------|
| `PIO_IOTYPE_ADIOS_SST` | 10 | ADIOS2 SST streaming engine (no disk file) |

**Fortran counterpart**:
```fortran
integer(i4), public, parameter :: PIO_iotype_adios_sst = 10
```

**Guard**: The new value is defined only within `#ifdef _ADIOS2` blocks (same as values
6 and 7).

**Validation rules**:
- A caller specifying `PIO_IOTYPE_ADIOS_SST` when `WITH_ADIOS2` is not built in receives
  `PIO_EADIOS2ERR` with a message identifying SST as the unavailable engine.
- A caller specifying `PIO_IOTYPE_ADIOS_SST` when the ADIOS2 library was not compiled
  with SST transport support receives the same error at stream-open time.

---

## Entity 2: File Descriptor (`file_desc_t`) — SST extensions

**Location**: `src/clib/pio_types.hpp` (existing structure, no new fields required)

**What it represents**: Per-open-file state. For SST streams, the same handle fields
(`engineH`, `ioH`, `adios_comm`, `filename`) are used as for BP streams, with different
semantics:

| Field | BP5 semantics | SST semantics |
|-------|--------------|---------------|
| `filename` | On-disk file path | Stream name (used as SST rendezvous key) |
| `engineH` | BP5 engine handle | SST engine handle |
| `ioH` | IO group handle (BP5) | IO group handle (SST) |
| `begin_step_called` | Step tracking for flush | Step tracking; SST blocks in begin_step until writer/reader rendezvous |
| `transport[PIO_MAX_NAME]` | Unused for BP | SST DataTransport value (e.g., "sockets", "WAN") |
| `params[PIO_MAX_NAME]` | Unused for BP | SST extra key=value parameters |

**SST-specific parameter fields** (existing `transport` and `params` fields repurposed):

| SCORPIO field | ADIOS2 SST key | Default |
|---------------|---------------|---------|
| `transport` | `DataTransport` | ADIOS2 default (MPI-based on HPC, sockets elsewhere) |
| `params` | Multi-key string, parsed to: | — |
| — | `MarshalMethod` | ADIOS2 default (`BP`) |
| — | `QueueLimit` | ADIOS2 default (0 = unlimited) |
| — | `QueueFullPolicy` | ADIOS2 default (`Block`) |
| — | `RendezvousReaderCount` | `1` (SCORPIO v1 single-reader constraint) |

**No new fields needed** in `file_desc_t` for v1. The existing `transport` and `params`
char arrays are sufficient for the required SST parameters.

---

## Entity 3: SST Stream

**What it represents**: The named, transient in-memory communication channel between one
SCORPIO writer and one SCORPIO reader. Exists in runtime only — no on-disk artifact is
created during normal operation.

**Identity**: The stream name is the string passed as `filename` to `PIOc_openfile` /
`PIOc_createfile`. Both writer and reader must use the same stream name.

**Lifecycle**:

```
Writer:  createfile (SST) → define variables → begin_step → write → end_step → closefile
                                                    ↑ (repeat N times) ↑
Reader:  openfile (SST)   → begin_step (blocks until writer steps) → read → end_step → closefile
```

**State transitions**:

| State | Condition | Next state |
|-------|-----------|-----------|
| Waiting for rendezvous | `adios2_open` called; no peer attached yet | Connected |
| Connected | Both writer and reader have opened the stream | Stepping |
| Stepping | `begin_step` / `end_step` cycling | Stepping or Closed |
| Rendezvous timeout | No peer attaches within timeout window | Error (SCORPIO reports `PIO_EADIOS2ERR` with stream name) |
| Closed | Writer or reader calls `closefile` | Terminated |
| Unexpected disconnect | Writer crashes before closing | Reader receives end-of-stream indication |

**Constraints**:
- Only one concurrent reader per stream is supported in v1.
- A second reader attempting to attach is not guaranteed to fail silently; behavior is
  undefined and MUST be documented as unsupported.

---

## Entity 4: Engine Configuration

**What it represents**: The set of SST-specific tunable parameters applied to a stream
at open time via `adios2_set_parameter`. These cannot be changed after `adios2_open`.

**Fields** (ADIOS2 SST parameter names):

| Parameter | Type | Description | SCORPIO v1 default |
|-----------|------|-------------|-------------------|
| `DataTransport` | string | Wire transport layer (`sockets`, `WAN`, `enet`, `ucx`, etc.) | ADIOS2 default |
| `MarshalMethod` | string | Serialization format (`BP`, `FFS`) | ADIOS2 default (`BP`) |
| `QueueLimit` | integer ≥ 0 | Max steps buffered before applying `QueueFullPolicy` | 0 (unlimited) |
| `QueueFullPolicy` | string | Behavior when queue full (`Block`, `Discard`) | `Block` |
| `RendezvousReaderCount` | integer | Expected number of readers at rendezvous | 1 |

**Validation rules**:
- If an invalid value is set for any parameter and ADIOS2 rejects it, SCORPIO MUST
  catch the resulting ADIOS2 error and surface it as a clear configuration error
  identifying the parameter name and stream name (per FR-004 acceptance scenario 2).

---

## Entity 5: Rendezvous

**What it represents**: The handshake period during which a writer waits for a reader
(or vice versa) to attach to a named stream.

**Behavior**:
- Controlled by ADIOS2 SST internally; exposed in SCORPIO via the `RendezvousReaderCount`
  parameter and any transport-specific timeout configuration.
- If the rendezvous window expires without both sides connecting, ADIOS2 returns an error
  to `adios2_open`. SCORPIO converts this to `PIO_EADIOS2ERR` with a message that
  includes the stream name and identifies it as a rendezvous timeout (FR-005).

**No persistent state**: Once both sides disconnect, the rendezvous context is destroyed.
Reconnection requires a fresh `openfile` / `createfile` cycle.
