# API Contracts: ADIOS2 SST Engine Support

**Branch**: `halehawk/adios2-sst-engine` | **Date**: 2026-09-01

This document defines the complete public interface changes introduced by SST engine
support. SCORPIO adds no new API functions; all SST functionality is accessed through
the existing file-open API with the new I/O type constant.

---

## Contract 1: C I/O Type Enum Extension

**File**: `src/clib/pio.h`

**Change**: Add one value to the existing `PIO_IOTYPE` enum within the `#ifdef _ADIOS2`
guard.

**Before**:
```c
#ifdef _ADIOS2
enum PIO_IOTYPE {
    ...
    PIO_IOTYPE_ADIOS  = 6,
    PIO_IOTYPE_ADIOSC = 7,
    PIO_IOTYPE_HDF5   = 8,
    PIO_IOTYPE_HDF5C  = 9
};
#endif
```

**After**:
```c
#ifdef _ADIOS2
enum PIO_IOTYPE {
    ...
    PIO_IOTYPE_ADIOS     = 6,
    PIO_IOTYPE_ADIOSC    = 7,
    PIO_IOTYPE_HDF5      = 8,
    PIO_IOTYPE_HDF5C     = 9,
    PIO_IOTYPE_ADIOS_SST = 10
};
#endif
```

**Usage contract**:
- Callers pass `PIO_IOTYPE_ADIOS_SST` as the `iotype` argument to `PIOc_createfile`
  (writer) or `PIOc_openfile` (reader).
- No other API changes. All existing write and read calls (`PIOc_put_var*`,
  `PIOc_get_var*`, `PIOc_write_darray`, `PIOc_read_darray`, etc.) work unchanged.
- The `filename` argument to `PIOc_createfile` / `PIOc_openfile` becomes the SST stream
  name (the rendezvous key). No `.bp` extension is required or meaningful.

**Error behavior**:
- If `_ADIOS2` is not defined at build time and a caller passes `PIO_IOTYPE_ADIOS_SST`:
  returns `PIO_EADIOS2ERR`.
- If ADIOS2 was not built with SST transport support, `adios2_open` fails; SCORPIO
  returns `PIO_EADIOS2ERR` with a message identifying SST and the stream name.

---

## Contract 2: Fortran I/O Type Constant

**File**: `src/flib/pio_types.F90`

**Change**: Add one integer parameter within the existing `#ifdef _ADIOS2` block.

**Before**:
```fortran
#ifdef _ADIOS2
integer(i4), public, parameter :: &
    PIO_iotype_adios  = 6, &
    PIO_iotype_adiosc = 7
#endif
```

**After**:
```fortran
#ifdef _ADIOS2
integer(i4), public, parameter :: &
    PIO_iotype_adios     = 6, &
    PIO_iotype_adiosc    = 7, &
    PIO_iotype_adios_sst = 10
#endif
```

**Usage contract**: Same semantics as the C constant. Pass `PIO_iotype_adios_sst` to
`pio_createfile` or `pio_openfile` via the Fortran interface in `src/flib/pio.F90`.

---

## Contract 3: I/O Type String Identifier (Test Framework)

**File**: `tests/general/util/pio_tutil.F90`

**Change**: Extend `PIO_TF_Iotype_from_str` to recognise the SST iotype string, and
extend `PIO_TF_Get_iotypes` to include `PIO_IOTYPE_ADIOS_SST` when `_ADIOS2` is defined.

**New string aliases accepted**:
- `"PIO_IOTYPE_ADIOS_SST"`
- `"pio_iotype_adios_sst"`
- `"ADIOS_SST"`

**Usage**: CTest arguments (`--pio-tf-targ-iotype=ADIOS_SST`) and test matrix generation
use these strings to target the SST engine.

---

## Contract 4: Iotype String Display

**File**: `src/clib/util/pio_print.cpp`

**Function**: `pio_iotype_to_string(int iotype)`

**Change**: Add a case for `PIO_IOTYPE_ADIOS_SST`:

```c
case PIO_IOTYPE_ADIOS_SST:
    return "PIO_IOTYPE_ADIOS_SST";
```

**Usage**: Error messages, debug logs, and diagnostic output that print the current
iotype will display `"PIO_IOTYPE_ADIOS_SST"` for SST streams.

---

## Contract 5: SST Engine Parameter Schema

**Mechanism**: SST-specific parameters are applied via `adios2_set_parameter` inside the
SST engine-setup block in `pioc_support.cpp`. User-visible configuration is through
SCORPIO's existing I/O system parameter mechanism (the `transport` and `params` fields
in `file_desc_t` for the SST-typed file descriptor).

**Supported parameters for v1**:

| Parameter key | Valid values | Default | Effect |
|---------------|-------------|---------|--------|
| `DataTransport` | `"sockets"`, `"enet"`, `"WAN"`, `"ucx"`, or ADIOS2 default | ADIOS2 default | Selects wire transport layer |
| `MarshalMethod` | `"BP"`, `"FFS"` | `"BP"` | Data serialization format |
| `QueueLimit` | Non-negative integer (as string) | `"0"` (unlimited) | Max buffered steps before policy triggers |
| `QueueFullPolicy` | `"Block"`, `"Discard"` | `"Block"` | Writer behavior when queue full |

**Error handling**: An invalid parameter value that causes `adios2_open` or the
subsequent first `adios2_begin_step` to fail MUST be surfaced as `PIO_EADIOS2ERR`
with a message identifying the stream name and, where ADIOS2 provides it, the
offending parameter key.
