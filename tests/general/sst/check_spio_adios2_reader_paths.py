#!/usr/bin/env python3
"""Check tools/adios2py/spio_adios2_reader.py against the BP file written by
pio_bp_pyreader_writer: block merging, several ADIOS steps, fill values,
non-record darrays, put_var arrays and scalars, record put_vara and char.

Usage: check_spio_adios2_reader_paths.py BP_FILE NPROCS

The expected values mirror pio_bp_pyreader_writer.c.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "tools", "adios2py"))
from spio_adios2_reader import SpioFile  # noqa: E402

NFRAMES, ELEMENTS_PER_PE, NCHARS, FILL_INT, LABEL = 300, 4, 8, -999, b"SCORPIO!"


def main():
    path, nprocs = sys.argv[1], int(sys.argv[2])
    nx = nprocs * ELEMENTS_PER_PE
    errors = []

    def check(cond, msg):
        if not cond:
            errors.append(msg)

    def check_array(name, data, values, dtype):
        check(data.dtype == np.dtype(dtype), f"{name}: dtype {data.dtype}, expected {np.dtype(dtype)}")
        check(data.shape == values.shape, f"{name}: shape {data.shape}, expected {values.shape}")
        if data.shape == values.shape:
            bad = np.argwhere(data != values)
            if bad.size:
                first = tuple(bad[0])
                errors.append(f"{name}: {len(bad)} wrong values, first at {first}: "
                              f"got {data[first]!r}, expected {values[first]!r}")

    f_idx = np.arange(NFRAMES)[:, None]
    x = np.arange(nx)
    sparse = np.where(x % 2 == 0, f_idx * 100 + x, FILL_INT)

    with SpioFile(path) as f:
        # The file really has the structure being tested
        check(f.adios_steps > 1, f"only {f.adios_steps} ADIOS step(s); expected several")
        check(any(len(g) > 1 for g in f.block_groups), f"no merged blocks: block groups {f.block_groups}")
        check(f.dims == {"time": NFRAMES, "x": nx, "nchars": NCHARS}, f"dims = {f.dims}")

        check_array("temp", f.read("temp"), f_idx * 1000.0 + x + 0.5, np.float64)
        check_array("sparse", f.read("sparse"), sparse, np.int32)
        check_array("mask", f.read("mask"), x % 3, np.int32)
        check_array("xcoord", f.read("xcoord"), 0.25 * x, np.float64)
        check_array("p0", f.read("p0"), np.array(1000.5), np.float64)
        check_array("step", f.read("step"), 7 * np.arange(NFRAMES) + 1, np.int32)
        label = f.read("label")
        check_array("label", label, np.frombuffer(LABEL, dtype="S1"), "S1")

    if errors:
        print("spio_adios2_reader paths check FAILED:")
        for e in errors:
            print("  " + e)
        return 1
    print(f"spio_adios2_reader paths check passed ({f.adios_steps} ADIOS steps, "
          f"block groups {f.block_groups})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
