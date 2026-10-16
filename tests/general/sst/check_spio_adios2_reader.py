#!/usr/bin/env python3
"""Check tools/adios2py/spio_adios2_reader.py against a BP file written by
pio_sst_multivar_writer with SPIO_SST_TEST_IOTYPE=bp.

Usage: check_spio_adios2_reader.py BP_FILE NPROCS

The expected values mirror pio_sst_multivar.h.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "tools", "adios2py"))
from spio_adios2_reader import SpioFile  # noqa: E402

NFRAMES, NY, ELEMENTS_PER_PE = 10, 3, 4


def main():
    path, nprocs = sys.argv[1], int(sys.argv[2])
    nx = nprocs * ELEMENTS_PER_PE
    errors = []

    def check(cond, msg):
        if not cond:
            errors.append(msg)

    f_idx = np.arange(NFRAMES)[:, None]
    x_idx = np.arange(nx)[None, :]
    expected = {
        "temp": (np.float64, ["time", "x"], f_idx * 1000.0 + x_idx + 0.5),
        "salt": (np.float64, ["time", "x"], -(f_idx * 1000.0 + x_idx) - 0.25),
        "pres": (np.int32, ["time", "y", "x"],
                 f_idx[:, :, None] * 10000 + np.arange(NY)[None, :, None] * 100 + x_idx[:, None, :]),
    }

    with SpioFile(path) as f:
        check(f.dims == {"time": NFRAMES, "x": nx, "y": NY}, f"dims = {f.dims}")
        check(f.unlimited == {"time"}, f"unlimited = {f.unlimited}")
        check(f.attrs.get("title") == "SCORPIO SST multivar test", f"title = {f.attrs.get('title')!r}")

        for name, (dtype, dims, values) in expected.items():
            check(name in f.variables, f"variable {name} missing")
            if name not in f.variables:
                continue
            var = f.variables[name]
            check(var.dims == dims, f"{name}: dims {var.dims}, expected {dims}")
            data = f.read(name)
            check(data.dtype == dtype, f"{name}: dtype {data.dtype}, expected {np.dtype(dtype)}")
            check(data.shape == values.shape, f"{name}: shape {data.shape}, expected {values.shape}")
            if data.shape == values.shape:
                bad = np.argwhere(data != values)
                check(bad.size == 0, f"{name}: {len(bad)} wrong values, first at {tuple(bad[0]) if bad.size else ''}"
                      f": got {data[tuple(bad[0])] if bad.size else ''}, "
                      f"expected {values[tuple(bad[0])] if bad.size else ''}")

        check(f.variables["temp"].attrs.get("units") == "K",
              f"temp:units = {f.variables['temp'].attrs.get('units')!r}")
        check(np.array_equal(np.atleast_1d(f.variables["pres"].attrs.get("valid_range")), [0, 1000000]),
              f"pres:valid_range = {f.variables['pres'].attrs.get('valid_range')!r}")

    if errors:
        print("spio_adios2_reader check FAILED:")
        for e in errors:
            print("  " + e)
        return 1
    print(f"spio_adios2_reader check passed: {len(expected)} variables x {NFRAMES} frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
