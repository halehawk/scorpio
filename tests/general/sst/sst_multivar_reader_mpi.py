"""MPI test reader for the SST stream written by pio_sst_multivar_writer, in the
style of SstReader_mpi.py: the low-level adios2.bindings API
(ADIOS(comm) -> DeclareIO -> SetEngine('Sst') -> Open -> BeginStep/Get/EndStep).

Unlike MURaM, SCORPIO does not write ADIOS2 global arrays that a rank can
SetSelection([start, count]) into. Each writer (or block group) puts its data
as a 1-D local block, plus /__pio__/ metadata: the decomposition map (1-based
global indices) and, per write, the frame and decomposition used. So in each
step every reader rank reads the blocks with SetBlockSelection, places them
with the decomposition map, and keeps its own slab of x, which it verifies
against the values in pio_sst_multivar.h.

As in SstReader_mpi.py, the data is written to Zarr, one store per output
time: OUTDIR/<stream>.<frame>.zarr with arrays temp(x), salt(x), pres(y, x).
Rank 0 creates the group and arrays (chunked by each rank's slab, gzip, with
_ARRAY_DIMENSIONS for xarray), all ranks Barrier, then each rank writes its
slab. At the end rank 0 reopens the stores and verifies them.

Run in the writer's directory (the reader finds it through <stream>.sst):
    mpiexec -n 2 ./pio_sst_multivar_writer STREAM &
    mpiexec -n 2 python sst_multivar_reader_mpi.py STREAM [OUTDIR]
"""
import os
import sys

from mpi4py import MPI
import numpy as np
import adios2.bindings as adios2
import zarr
from zarr.storage import LocalStore

# Must match pio_sst_multivar.h
NFRAMES = 10
NY = 3
TITLE = "SCORPIO SST multivar test"

PIO = "/__pio__/"
NP_TYPES = {
    "int8_t": np.int8, "int16_t": np.int16, "int32_t": np.int32, "int64_t": np.int64,
    "uint8_t": np.uint8, "uint16_t": np.uint16, "uint32_t": np.uint32, "uint64_t": np.uint64,
    "float": np.float32, "double": np.float64, "char": np.int8,
}


def expected(name, frame, y, x):
    if name == "temp":
        return frame * 1000.0 + x + 0.5
    if name == "salt":
        return -(frame * 1000.0 + x) - 0.25
    return frame * 10000 + y * 100 + x                     # pres


stream_name = sys.argv[1] if len(sys.argv) > 1 else "scorpio_sst_multivar_stream"
outdir = sys.argv[2] if len(sys.argv) > 2 else "zarr_out"

# MPI
comm = MPI.COMM_WORLD
rank = comm.Get_rank()
nprocs = comm.Get_size()

# Zarr compressor
zarr_compression_kw = dict(compressors=zarr.codecs.GzipCodec(level=4))
if rank == 0:
    os.makedirs(outdir, exist_ok=True)


def store_path(frame):
    return os.path.join(outdir, f"{stream_name}.{frame:06d}.zarr")


def slab(nx):
    """This rank's x range: whole chunks, so no two ranks write the same chunk."""
    chunk_x = -(-nx // nprocs)
    x0 = min(nx, rank * chunk_x)
    return chunk_x, x0, min(nx, x0 + chunk_x)


def write_zarr(frame, vname, shape, dims, attrs, mine):
    """Rank 0 creates the store and array, then every rank writes its slab
    (all ranks call this in the same order)."""
    chunk_x, x0, x1 = slab(shape[-1])
    if rank == 0:
        grp = zarr.open_group(store=LocalStore(store_path(frame)), mode="a")
        grp.attrs["frame"] = frame
        if vname not in grp:
            arr = grp.create_array(vname, shape=shape, chunks=shape[:-1] + (chunk_x,),
                                   dtype=mine.dtype, fill_value=0, **zarr_compression_kw)
            arr.attrs["_ARRAY_DIMENSIONS"] = dims
            for key, value in attrs.items():
                arr.attrs[key] = value
    comm.Barrier()
    if x1 > x0:
        grp = zarr.open_group(store=LocalStore(store_path(frame)), mode="a")
        grp[vname][..., x0:x1] = mine

# ADIOS MPI communicator
adios = adios2.ADIOS(comm)

# ADIOS IO
sstIO = adios.DeclareIO("scorpio_sst_reader")
# ADIOS engine
sstIO.SetEngine("Sst")
sstIOParams = {}
sstIOParams["OpenTimeoutSecs"] = "60"
sstIO.SetParameters(sstIOParams)

sstReader = sstIO.Open(stream_name, adios2.Mode.Read)


# --------------------------------------------------------------------------- helpers
def num_blocks(name):
    return len(sstReader.BlocksInfo(name, sstReader.CurrentStep()))


def get_block(name, block):
    var = sstIO.InquireVariable(name)
    var.SetBlockSelection(block)
    buf = np.zeros(var.SelectionSize(), dtype=NP_TYPES[var.Type()])
    sstReader.Get(var, buf, adios2.Mode.Sync)
    return buf


def get_blocks(name, avail):
    return [get_block(name, b) for b in range(num_blocks(name))] if name in avail else []


def get_concat(name, avail):
    blocks = get_blocks(name, avail)
    return np.concatenate(blocks) if blocks else np.array([], dtype=np.int64)


def get_value(name):
    var = sstIO.InquireVariable(name)
    buf = np.zeros(1, dtype=NP_TYPES[var.Type()])
    sstReader.Get(var, buf, adios2.Mode.Sync)
    return buf[0]


def get_string(name):
    return sstReader.Get(sstIO.InquireVariable(name), adios2.Mode.Sync)


def get_attr(name):
    attr = sstIO.InquireAttribute(name)
    if not attr:
        return None
    if attr.Type() == "string":
        value = attr.DataString()
        return value[0] if len(value) == 1 else value
    value = np.asarray(attr.Data()).ravel()
    return value.item() if value.size == 1 else value


# --------------------------------------------------------------------------- read the stream
errors = []
block_list = None
dims = {}
decomps = {}                       # decomp id -> (1-based map, global shape)
frames_seen = {"temp": set(), "salt": set(), "pres": set()}
checked = 0

while True:
    status = sstReader.BeginStep(adios2.StepMode.Read, 60.0)
    if status != adios2.StepStatus.OK:
        break
    avail = sstIO.AvailableVariables()

    # Writer ranks in each block group, and dimension lengths
    if block_list is None and PIO + "info/block_list" in avail:
        block_list = [[int(w) for w in b] for b in get_blocks(PIO + "info/block_list", avail)]
    for name in avail:
        if name.startswith(PIO + "dim/"):
            dims[name[len(PIO + "dim/"):]] = int(get_value(name))

    # Decomposition maps, in block-group order (see ProcessOneDecomposition)
    for name in avail:
        if name.startswith(PIO + "decomp/") and name.count("/") == 3:
            did = int(name.rsplit("/", 1)[1])
            if did in decomps:
                continue
            counts = get_blocks(PIO + "track/num_decomp_block_writers/" + str(did), avail)
            has_block = sorted(w for g, group in enumerate(block_list)
                               for w in group[:int(counts[g][0])])
            block_of = {w: i for i, w in enumerate(has_block)}
            dmap = np.concatenate([get_block(name, block_of[w])
                                   for group in block_list for w in group if w in block_of])
            shape = tuple(int(d) for d in np.atleast_1d(get_attr(name + "/dimlen")))
            decomps[did] = (dmap.astype(np.int64), shape)

    for vname in frames_seen:
        frames = get_concat(PIO + "track/frame_id/" + vname, avail)
        if frames.size == 0:
            continue
        decomp_ids = get_concat(PIO + "track/decomp_id/" + vname, avail)

        # Dimension names: on SST, from /__pio__/sst/vardims (the /def/dims
        # string-array attribute arrives empty, see SstWriter::MarshalAttributes)
        ndims = int(get_attr(PIO + "var/" + vname + "/def/ndims"))
        vdims = [get_string(f"{PIO}sst/vardims/{vname}/{i}") for i in range(ndims)]
        want = ["time", "y", "x"] if vname == "pres" else ["time", "x"]
        if vdims != want:
            errors.append(f"{vname}: dims {vdims}, expected {want}")

        # Which (writer, write) pairs have a data block, numbered writer by writer
        counts = get_blocks(PIO + "track/num_data_block_writers/" + vname, avail)
        per_group = len(counts) // len(block_list)
        present = set()
        b_idx = 0
        for group in block_list:
            t = 0
            for _ in range(per_group):
                for nwriters in counts[b_idx]:
                    present.update((group[k], t) for k in range(int(nwriters)))
                    t += 1
                b_idx += 1
        block_of = {}
        for w in range(sum(len(g) for g in block_list)):
            for t in range(frames.size):
                if (w, t) in present:
                    block_of[(w, t)] = len(block_of)

        for t in range(frames.size):
            frame = int(frames[t])
            dmap, shape = decomps[abs(int(decomp_ids[t]))]
            data = np.concatenate([get_block(PIO + "var/" + vname, block_of[(w, t)])
                                   for group in block_list for w in group if (w, t) in block_of])
            glob = np.zeros(int(np.prod(shape)), dtype=data.dtype)
            n = min(dmap.size, data.size)
            valid = dmap[:n] > 0
            glob[dmap[:n][valid] - 1] = data[:n][valid]
            glob = glob.reshape(shape)

            # This rank's slab of x
            _, x0, x1 = slab(shape[-1])
            mine = glob[..., x0:x1]
            x = np.arange(x0, x1)
            want = (expected(vname, frame, np.arange(NY)[:, None], x[None, :]) if mine.ndim == 2
                    else expected(vname, frame, 0, x))
            bad = np.argwhere(mine != want)
            if bad.size:
                errors.append(f"{vname} frame {frame}: {len(bad)} wrong values on rank {rank}, "
                              f"first got {mine[tuple(bad[0])]} expected {want[tuple(bad[0])]}")
            frames_seen[vname].add(frame)
            checked += mine.size

            # Write this frame's slab to Zarr, with the variable's NetCDF attributes
            prefix = PIO + "var/" + vname + "/"
            vattrs = {}
            for aname in sstIO.AvailableAttributes():
                if aname.startswith(prefix) and "/" not in aname[len(prefix):]:
                    value = get_attr(aname)
                    vattrs[aname[len(prefix):]] = value.tolist() if isinstance(value, np.ndarray) else value
            write_zarr(frame, vname, shape, vdims[1:], vattrs, mine)

    # Attributes (single-value ones arrive intact over SST)
    if rank == 0 and get_attr(PIO + "global/title") is not None:
        if get_attr(PIO + "global/title") != TITLE:
            errors.append(f"title = {get_attr(PIO + 'global/title')!r}")
        if get_attr(PIO + "var/temp/units") != "K":
            errors.append(f"temp:units = {get_attr(PIO + 'var/temp/units')!r}")
        if not np.array_equal(get_attr(PIO + "var/pres/valid_range"), [0, 1000000]):
            errors.append(f"pres:valid_range = {get_attr(PIO + 'var/pres/valid_range')!r}")

    sstReader.EndStep()

sstReader.Close()

for vname, seen in frames_seen.items():
    if seen != set(range(NFRAMES)):
        errors.append(f"{vname}: frames {sorted(seen)}, expected 0..{NFRAMES - 1}")
if dims.get("y") != NY:
    errors.append(f"dims = {dims}")

# Reopen the Zarr stores and check what is on disk
comm.Barrier()
if rank == 0:
    nx = dims.get("x", 0)
    x = np.arange(nx)
    for frame in range(NFRAMES):
        try:
            grp = zarr.open_group(store=LocalStore(store_path(frame)), mode="r")
            for vname, want in (("temp", expected("temp", frame, 0, x)),
                                ("salt", expected("salt", frame, 0, x)),
                                ("pres", expected("pres", frame, np.arange(NY)[:, None], x[None, :]))):
                got = grp[vname][...]
                if got.shape != want.shape or not np.array_equal(got, want):
                    errors.append(f"zarr {store_path(frame)}:{vname} does not match")
            if grp["temp"].attrs.get("units") != "K" or grp["pres"].attrs.get("_ARRAY_DIMENSIONS") != ["y", "x"]:
                errors.append(f"zarr {store_path(frame)}: attributes {dict(grp['temp'].attrs)}, "
                              f"{dict(grp['pres'].attrs)}")
        except Exception as e:                     # missing store or array
            errors.append(f"zarr {store_path(frame)}: {e!r}")

all_errors = comm.gather(errors, root=0)
total_checked = comm.reduce(checked, op=MPI.SUM, root=0)
if rank == 0:
    flat = [e for errs in all_errors for e in errs]
    if flat:
        print("SST Python MPI reader FAILED:")
        for e in flat:
            print("  " + e)
    else:
        print(f"SST Python MPI reader passed: {nprocs} ranks verified {total_checked} values, "
              f"{NFRAMES} frames x 3 variables, block groups {block_list}; "
              f"Zarr stores verified in {outdir}/")
failed = comm.bcast(bool(rank == 0 and flat), root=0)
sys.exit(1 if failed else 0)
