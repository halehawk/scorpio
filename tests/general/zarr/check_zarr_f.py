"""Check the Zarr v3 store written by pio_zarr_f.F90 (PIO_IOTYPE_ZARR).

    python check_zarr_f.py <store> [nprocs]

Reads the store with zarr-python (and xarray when available) and checks
the metadata, chunk shapes and every value against the formulas in
pio_zarr_f.F90. Exits non-zero on the first failure.
"""

import base64
import json
import os
import struct
import sys

import numpy as np
import zarr

NLAT, NLON, NFRAMES = 256, 512, 3
NC_FILL_INT = -2147483647

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def main():
    store = sys.argv[1]
    nprocs = sys.argv[2] if len(sys.argv) > 2 else "?"
    print(f"checking {store} (written by {nprocs} ranks)")

    with open(os.path.join(store, "zarr.json")) as fh:
        meta = json.load(fh)
    check(meta["zarr_format"] == 3 and meta["node_type"] == "group", "root is a Zarr v3 group")

    g = zarr.open_group(store, mode="r")
    check(g.attrs["title"] == "SCORPIO native Zarr Fortran test", "global attribute title")
    check(g.attrs["version"] == 3, "global attribute version")
    check(sorted(g.array_keys()) == sorted(["lat", "lon", "time", "scale", "T", "Tf", "U", "mask"]),
          "variables")

    f = np.arange(NFRAMES, dtype="f8")[:, None, None]
    j = np.arange(NLAT, dtype="f8")[None, :, None]
    i = np.arange(NLON, dtype="f8")[None, None, :]
    t_ref = 250.0 + f + 0.01 * j + 0.0001 * i
    u_ref = 10.0 * f + j - 0.5 * i

    expect = {
        #  name     shape                     chunks           dtype      dims
        "T":    ((NFRAMES, NLAT, NLON), (1, 128, 256), "float64", ["time", "lat", "lon"]),
        "Tf":   ((NFRAMES, NLAT, NLON), (1, 128, 256), "float32", ["time", "lat", "lon"]),
        "U":    ((NFRAMES, NLAT, NLON), (1, 64, 100),  "float64", ["time", "lat", "lon"]),
        "mask": ((NLAT, NLON),          (128, 512),    "int32",   ["lat", "lon"]),
        "lat":  ((NLAT,),               (NLAT,),       "float64", ["lat"]),
        "lon":  ((NLON,),               (NLON,),       "float64", ["lon"]),
        "time": ((NFRAMES,),            (1,),          "float64", ["time"]),
        "scale": ((),                   (),            "float64", []),
    }
    for name, (shape, chunks, dtype, dims) in expect.items():
        a = g[name]
        check(a.shape == shape, f"{name} shape {a.shape}")
        check(a.chunks == chunks, f"{name} chunks {a.chunks}")
        check(str(a.dtype) == dtype, f"{name} dtype {a.dtype}")
        check(list(a.metadata.dimension_names or []) == dims, f"{name} dimension_names")

    check(np.array_equal(g["T"][:], t_ref), "T values")
    check(np.array_equal(g["Tf"][:], t_ref.astype("f4")), "Tf values (double -> float)")
    check(np.array_equal(g["U"][:], u_ref), "U values (user chunks)")

    jj, ii = np.meshgrid(np.arange(NLAT), np.arange(NLON), indexing="ij")
    mask_ref = np.where((ii + jj) % 7 == 0, NC_FILL_INT, jj * NLON + ii).astype("i4")
    check(np.array_equal(g["mask"][:], mask_ref), "mask values (holes are the fill value)")
    check(g["mask"].fill_value == NC_FILL_INT, "mask fill_value (NetCDF default)")

    check(np.array_equal(g["lat"][:], -90.0 + np.arange(NLAT) * (180.0 / (NLAT - 1))), "lat values")
    check(np.array_equal(g["lon"][:], np.arange(NLON) * (360.0 / NLON)), "lon values")
    check(np.array_equal(g["time"][:], 6.0 * np.arange(NFRAMES)), "time values")
    check(g["scale"][()] == 2.5, "scale value")

    check(g["T"].attrs["units"] == "K", "T units")
    # Float _FillValue attributes are base64 little-endian doubles, as xarray
    # writes them in Zarr v3 stores
    fill_attr = struct.unpack("<d", base64.standard_b64decode(g["T"].attrs["_FillValue"]))[0]
    check(fill_attr == -999.0 and g["T"].fill_value == -999.0,
          "T _FillValue attribute and Zarr fill_value")
    check(g["U"].attrs["valid_range"] == [-300.0, 300.0], "U valid_range")

    # One chunk file per chunk: 4 per T record (2 x 2 chunk grid)
    t_chunks = [os.path.join(r, n) for r, _, ns in os.walk(os.path.join(store, "T", "c"))
                for n in ns]
    check(len(t_chunks) == NFRAMES * 4, f"T chunk files ({len(t_chunks)})")

    try:
        import xarray as xr
    except ImportError:
        print("xarray not available, skipping the xarray checks")
    else:
        ds = xr.open_zarr(store, consolidated=False, zarr_format=3)
        check(ds.sizes == {"time": NFRAMES, "lat": NLAT, "lon": NLON}, f"xarray sizes {dict(ds.sizes)}")
        check(set(ds.coords) >= {"time", "lat", "lon"}, "xarray coordinates")
        check(np.array_equal(ds["T"].values, t_ref), "xarray T values")
        check(ds["T"].attrs.get("units") == "K", "xarray T units")
        check(ds.attrs.get("title") == "SCORPIO native Zarr Fortran test", "xarray title")

    if failures:
        print(f"FAIL: {failures} check(s) failed")
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
