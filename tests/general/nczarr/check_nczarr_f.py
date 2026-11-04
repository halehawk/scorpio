"""Check the Zarr store written by pio_nczarr_f with zarr-python, i.e.
without NetCDF or SCORPIO: what any Zarr reader would see.

    python check_nczarr_f.py pio_nczarr_f.zarr <nprocs>
"""
import sys

import numpy as np
import zarr

path, nprocs = sys.argv[1], int(sys.argv[2])
NLEV, LON_PER_PE, NFRAMES = 3, 4, 4
nlon = nprocs * LON_PER_PE

results = []


def check(what, ok):
    results.append(bool(ok))
    print(("PASS" if ok else "FAIL"), what)


# NCZarr writes Zarr format 2
g = zarr.open_group(path, mode="r", zarr_format=2)
check(f"arrays {sorted(n for n, _ in g.arrays())}",
      sorted(n for n, _ in g.arrays()) == ["T", "lon", "ps"])
check("global attribute title", g.attrs.get("title") == "SCORPIO NCZarr Fortran test")

f = np.arange(NFRAMES)[:, None, None]
k = np.arange(NLEV)[None, :, None]
x = np.arange(nlon)[None, None, :]
T_want = 250.0 + f + 10.0 * k + 0.01 * x
ps_want = (np.float32(1000.0) + np.arange(NFRAMES, dtype=np.float32)[:, None]
           + np.float32(0.5) * np.arange(nlon, dtype=np.float32)[None, :])

T = g["T"]
check(f"T shape {T.shape}", T.shape == (NFRAMES, NLEV, nlon))
check(f"T dtype {T.dtype}", T.dtype == np.float64)
check(f"T chunks {T.chunks}", T.chunks == (1, 2, LON_PER_PE))
check(f"T dims {T.attrs.get('_ARRAY_DIMENSIONS')}",
      T.attrs.get("_ARRAY_DIMENSIONS") == ["time", "lev", "lon"])
check("T units/long_name",
      T.attrs.get("units") == "K" and T.attrs.get("long_name") == "air temperature")
check("T values", np.array_equal(T[...], T_want))

ps = g["ps"]
check(f"ps shape {ps.shape}, dtype {ps.dtype}", ps.shape == (NFRAMES, nlon) and ps.dtype == np.float32)
check(f"ps dims {ps.attrs.get('_ARRAY_DIMENSIONS')}", ps.attrs.get("_ARRAY_DIMENSIONS") == ["time", "lon"])
check("ps units", ps.attrs.get("units") == "hPa")
check("ps values", np.array_equal(ps[...], ps_want))

lon = g["lon"]
check("lon values", np.array_equal(lon[...], 0.5 * np.arange(nlon)))

try:
    import xarray as xr
    ds = xr.open_zarr(path, consolidated=False, zarr_format=2)
    check(f"xarray opens it: {dict(ds.sizes)}",
          dict(ds.sizes) == {"time": NFRAMES, "lev": NLEV, "lon": nlon}
          and np.array_equal(ds["T"].values, T_want))
except ImportError:
    print("SKIP xarray not installed")

print(f"{sum(results)} passed, {len(results) - sum(results)} failed")
sys.exit(0 if all(results) else 1)
