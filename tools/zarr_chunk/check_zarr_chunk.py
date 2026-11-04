"""Check the stores written by zarr_chunk_test against zarr-python.

For each array, zarr-python must read back the same values, and when
zarr-python writes the same data with the same chunks (no compression),
it must produce the same chunk files with the same bytes.
"""
import os
import sys

import numpy as np
import zarr

outdir = sys.argv[1]


def expected():
    f, y, x = np.meshgrid(np.arange(10), np.arange(3), np.arange(8), indexing="ij")
    pres = (f * 10000 + y * 100 + x).astype(np.int32)
    sparse = np.full((7, 5), np.nan)
    sparse[:3] = 0.5 * np.arange(15).reshape(3, 5)
    small = np.arange(1, 6, dtype=np.float32)
    arrays = {
        "pres.zarr": (pres, (4, 2, 3), -1),
        "sparse.zarr": (sparse, (3, 2), np.nan),
        "small.zarr": (small, (8,), 0.0),
        # Written only by zarr_chunk_mpi_test, with other MPI layouts of pres
        "pres_t.zarr": (pres, (4, 2, 3), -1),
        "pres_one.zarr": (pres, (4, 2, 3), -1),
        "pres_multi.zarr": (pres, (4, 2, 3), -1),
    }
    return {k: v for k, v in arrays.items() if os.path.isdir(os.path.join(outdir, k))}


def chunk_files(path):
    files = {}
    for root, _, names in os.walk(os.path.join(path, "c")):
        for n in names:
            p = os.path.join(root, n)
            with open(p, "rb") as fh:
                files[os.path.relpath(p, path)] = fh.read()
    return files


failed = False
for name, (data, chunks, fill) in expected().items():
    path = os.path.join(outdir, name)
    got = zarr.open_array(path, mode="r")[...]
    same_values = np.array_equal(got, data, equal_nan=True)

    ref_path = os.path.join(outdir, "ref_" + name)
    ref = zarr.create_array(ref_path, shape=data.shape, chunks=chunks, dtype=data.dtype,
                            fill_value=fill, compressors=None, overwrite=True)
    ref[...] = data
    mine, theirs = chunk_files(path), chunk_files(ref_path)
    same_chunks = mine == theirs

    ok = same_values and same_chunks
    failed |= not ok
    print(f"{'PASS' if ok else 'FAIL'} {name}: values {same_values}, "
          f"chunk files {same_chunks} ({sorted(mine)})")
    if not same_chunks:
        print("  only in C++:", sorted(set(mine) - set(theirs)))
        print("  only in zarr-python:", sorted(set(theirs) - set(mine)))

sys.exit(1 if failed else 0)
