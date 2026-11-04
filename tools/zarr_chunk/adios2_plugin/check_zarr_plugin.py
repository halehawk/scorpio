"""Check the Zarr group written by zarr_plugin_writer through the plugin.

For each array: values, chunk shape, fill value, dimension names and
attributes; and its chunk files must be byte for byte the ones zarr-python
writes for the same data and chunks (no compression).
"""
import os
import sys

import numpy as np
import zarr

path = sys.argv[1]
NSTEPS, NY, NX, NT, UY, UX = 4, 3, 8, 10, 6, 5
s = np.arange(NSTEPS)

y, x = np.meshgrid(np.arange(NY), np.arange(NX), indexing="ij")
pres = (s[:, None, None] * 10000 + y * 100 + x).astype(np.int32)
temp = s[:, None] * 100.0 + 0.5 * np.arange(NT)
p0 = 1000.0 + s
u = (s[:, None, None] + 0.25 * np.arange(UY * UX).reshape(UY, UX)).astype(np.float32)

expected = {
    #  name     data   chunks      fill     dims                         attrs
    "pres": (pres, (1, 2, 3), -1, ["step", "y", "x"], {"units": "Pa", "scale": 3}),
    "temp": (temp, (1, 4), np.nan, ["step", "temp_dim0"], {"valid_range": [0.0, 100.0]}),
    "p0": (p0, (1,), np.nan, ["step"], {}),
    "atm/u": (u, (1, UY, UX), np.nan, ["step", "u_dim0", "u_dim1"], {}),
}


def chunk_files(p):
    files = {}
    for root, _, names in os.walk(os.path.join(p, "c")):
        for n in names:
            f = os.path.join(root, n)
            with open(f, "rb") as fh:
                files[os.path.relpath(f, p)] = fh.read()
    return files


grp = zarr.open_group(path, mode="r")
failed = False
checks = [("group attrs", dict(grp.attrs) == {"title": "ZarrWrite plugin test"}),
          ("skipped loc/label", "loc" not in grp and "label" not in grp)]
for name, (data, chunks, fill, dims, attrs) in expected.items():
    a = grp[name]
    checks += [
        (f"{name} values", np.array_equal(a[...], data, equal_nan=True)),
        (f"{name} chunks", a.chunks == chunks),
        (f"{name} fill", np.array_equal(a.fill_value, fill, equal_nan=True)),
        (f"{name} dims", list(a.metadata.dimension_names) == dims),
        (f"{name} attrs", dict(a.attrs) == attrs),
    ]
    ref = zarr.create_array(os.path.join(path + ".ref", name), shape=data.shape, chunks=chunks,
                            dtype=data.dtype, fill_value=fill, compressors=None, overwrite=True)
    ref[...] = data
    checks.append((f"{name} chunk files", chunk_files(os.path.join(path, name))
                   == chunk_files(os.path.join(path + ".ref", name))))

for what, ok in checks:
    failed |= not ok
    print(("PASS" if ok else "FAIL"), what)
sys.exit(1 if failed else 0)
