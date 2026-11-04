"""Check that a Zarr store written through the ZarrWrite ADIOS2 plugin holds
the same data as the BP5 file written by the same application.

    python compare_zarr_bp.py backend_test.bp backend_test.zarr

Every step of every variable in the BP5 file must equal that step of the
Zarr array (Zarr arrays have a leading "step" dimension); steps where a
variable was not written must be all fill value in Zarr. Also checks the
Zarr metadata set in zarr_backend.xml and the attributes.
"""
import sys

import adios2
import numpy as np
import zarr

bp_path, zarr_path = sys.argv[1], sys.argv[2]

# What zarr_backend.xml asks for: name -> (chunks, dims, fill)
EXPECTED_META = {
    "T": ((1, 2, 3, 4), ["step", "lev", "lat", "lon"], np.nan),
    "ps": ((1, 4, 5), ["step", "lat", "lon"], np.nan),
    "time": ((1,), ["step"], np.nan),
    "mask": ((1, 6, 10), ["step", "lat", "lon"], 255),
    "flux": ((1, 3), ["step", "lon"], 0),  # "nan" parsed for an integer gives 0
}

results = []


def check(what, ok):
    results.append(ok)
    print(("PASS" if ok else "FAIL"), what)


# Read every step of the BP5 file.
bp_steps, bp_attrs = [], {}
with adios2.Stream(bp_path, "r") as f:
    for _ in f.steps():
        bp_steps.append({name: f.read(name) for name in f.available_variables()})
        for name in f.available_attributes():
            bp_attrs[name] = f.read_attribute(name)
nsteps = len(bp_steps)

grp = zarr.open_group(zarr_path, mode="r")
check(f"Zarr arrays are the BP5 variables {sorted(EXPECTED_META)}",
      sorted(name for name, _ in grp.arrays()) == sorted(EXPECTED_META))

for name, (chunks, dims, fill) in EXPECTED_META.items():
    a = grp[name]
    z = a[...]
    check(f"{name}: shape {a.shape} has {nsteps} steps", a.shape[0] == nsteps)
    check(f"{name}: chunks {a.chunks}", a.chunks == chunks)
    check(f"{name}: dims {list(a.metadata.dimension_names)}",
          list(a.metadata.dimension_names) == dims)
    check(f"{name}: fill {a.fill_value}", np.array_equal(a.fill_value, fill, equal_nan=True))
    same = True
    for s, step in enumerate(bp_steps):
        if name in step:
            same &= z[s].dtype == step[name].dtype and np.array_equal(z[s], step[name])
        else:  # not written in this step: all fill
            same &= bool(np.all(np.isnan(z[s]) if np.isnan(fill) else z[s] == fill))
    check(f"{name}: all steps equal BP5 (written in "
          f"{sum(name in st for st in bp_steps)} of {nsteps} steps)", same)


def norm(v):
    """BP attribute values as Zarr (JSON) would hold them."""
    if isinstance(v, np.ndarray):
        return v.item() if v.size == 1 and v.ndim == 0 else v.tolist()
    return v


for full, value in bp_attrs.items():
    if "/" in full:
        var, key = full.split("/", 1)
        got = grp[var].attrs.get(key)
    else:
        got = grp.attrs.get(full)
    want = norm(value)
    if isinstance(want, list) and len(want) == 1 and not isinstance(got, list):
        want = want[0]
    same = got == want if isinstance(want, str) else np.allclose(got, want)
    check(f"attribute {full} = {want!r}", bool(same))

try:
    import xarray as xr
    ds = xr.open_zarr(zarr_path, consolidated=False)
    check(f"xarray opens it: {dict(ds.sizes)}",
          dict(ds.sizes) == {"step": nsteps, "lev": 4, "lat": 6, "lon": 10})
except ImportError:
    print("SKIP xarray not installed")

print(f"{sum(results)} passed, {len(results) - sum(results)} failed")
sys.exit(0 if all(results) else 1)
