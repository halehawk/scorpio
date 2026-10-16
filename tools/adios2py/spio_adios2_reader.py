#!/usr/bin/env python3
"""Read ADIOS2 BP files written by SCORPIO (PIO_IOTYPE_ADIOS) with the ADIOS2
Python API, without SCORPIO, MPI or a NetCDF conversion.

SCORPIO does not store NetCDF variables as ADIOS2 global arrays. Each writer
rank (or block group, with block merging) puts its local data as a 1-D block,
and SCORPIO adds metadata under /__pio__/ describing dimensions, variables,
decompositions and which block belongs to which writer and frame. This module
follows the same rules as SCORPIO's BP-to-NetCDF converter
(tools/adios2pio-nm) to rebuild each variable as a numpy array with its
NetCDF shape.

Module use:
    from spio_adios2_reader import SpioFile
    with SpioFile("out.nc") as f:          # "out.nc" or "out.nc.bp"
        print(f.dims, list(f.variables))
        temp = f.read("temp")               # e.g. shape (time, x)

Command line (header, like "ncdump -h", plus values with -v):
    python spio_adios2_reader.py out.nc [-v VAR [VAR ...]]
"""

import argparse
import os
import sys

import numpy as np
import adios2

VAR_PREFIX = "/__pio__/var/"
DIM_PREFIX = "/__pio__/dim/"
GLOBAL_PREFIX = "/__pio__/global/"
DECOMP_PREFIX = "/__pio__/decomp/"
TRACK_PREFIX = "/__pio__/track/"

PIO_UNLIMITED = 0

# NetCDF type code -> (numpy dtype, NetCDF default fill value, CDL name)
NC_TYPES = {
    1: (np.int8, -127, "byte"),
    2: (np.dtype("S1"), b"\x00", "char"),
    3: (np.int16, -32767, "short"),
    4: (np.int32, -2147483647, "int"),
    5: (np.float32, 9.9692099683868690e36, "float"),
    6: (np.float64, 9.9692099683868690e36, "double"),
    7: (np.uint8, 255, "ubyte"),
    8: (np.uint16, 65535, "ushort"),
    9: (np.uint32, 4294967295, "uint"),
    10: (np.int64, -9223372036854775806, "int64"),
    11: (np.uint64, 18446744073709551614, "uint64"),
}


class SpioVariable:
    """Metadata of one NetCDF variable stored in a SCORPIO BP file."""

    def __init__(self, name, nctype, dims, op, attrs):
        self.name = name
        self.nctype = nctype      # NetCDF type code, e.g. 4 = NC_INT
        self.dims = dims          # dimension names, slowest first
        self.op = op              # "darray" or "put_var"
        self.attrs = attrs        # {attribute name: value}

    @property
    def dtype(self):
        return np.dtype(NC_TYPES.get(self.nctype, (np.float64,))[0])

    def __repr__(self):
        return f"SpioVariable({self.name!r}, nctype={self.nctype}, dims={self.dims}, op={self.op!r})"


class SpioFile:
    """A SCORPIO ADIOS2 BP file opened for reading."""

    def __init__(self, path):
        self.path = self._bp_path(path)
        self._f = adios2.FileReader(self.path)
        self._nsteps = self._f.num_steps()
        self._avail_vars = self._f.available_variables()
        self._avail_attrs = self._f.available_attributes()
        self._blocks_info = {}
        self._decomps = {}

        self.attrs = self._read_global_attrs()
        self.variables = self._read_variables()
        self._block_list = self._read_block_list()
        self._num_writers = sum(len(group) for group in self._block_list)
        self.unlimited, self.dims = self._read_dims()

    # ------------------------------------------------------------------ open/close
    @staticmethod
    def _bp_path(path):
        """Accept the NetCDF-style name SCORPIO was given or the .bp directory."""
        path = path.rstrip("/")
        if os.path.isdir(path):
            return path
        if os.path.isdir(path + ".bp"):
            return path + ".bp"
        raise FileNotFoundError(f"No ADIOS2 BP directory found for {path!r}")

    @property
    def adios_steps(self):
        """Number of ADIOS steps in the file (SCORPIO ends one every
        PIO_MAX_CACHED_STEPS_FOR_ADIOS frame changes)."""
        return self._nsteps

    @property
    def block_groups(self):
        """Writer ranks merged into each ADIOS block, one list per block group."""
        return [list(group) for group in self._block_list]

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ------------------------------------------------------------------ low-level ADIOS reads
    def _has_var(self, name):
        return name in self._avail_vars

    def _block_counts(self, name):
        """Per file step, the element count of each block of a local array."""
        if name not in self._blocks_info:
            counts = []
            for step_blocks in self._f.all_blocks_info(name):
                step_counts = []
                for block in step_blocks:
                    dims = [int(c) for c in str(block.get("Count", "0")).split(",") if c.strip()]
                    step_counts.append(int(np.prod(dims)) if dims else 1)
                counts.append(step_counts)
            counts += [[] for _ in range(self._nsteps - len(counts))]
            self._blocks_info[name] = counts
        return self._blocks_info[name]

    def _nblocks(self, name, step):
        return len(self._block_counts(name)[step]) if self._has_var(name) else 0

    def _read_block(self, name, step, block):
        return np.asarray(self._f.read(name, block_id=block, step_selection=[step, 1])).ravel()

    def _read_step_blocks(self, name, step):
        return [self._read_block(name, step, b) for b in range(self._nblocks(name, step))]

    def _read_step_concat(self, name, step):
        blocks = self._read_step_blocks(name, step)
        return np.concatenate(blocks) if blocks else np.array([], dtype=np.int64)

    def _first_step_with_blocks(self, name):
        for step in range(self._nsteps):
            if self._nblocks(name, step) > 0:
                return step
        return None

    def _read_value(self, name):
        """A single value written once (e.g. a dimension length)."""
        step = self._first_step_with_blocks(name)
        if step is None:
            step = 0
        return np.asarray(self._f.read(name, step_selection=[step, 1])).ravel()[0]

    def _attr(self, name, default=None):
        if name not in self._avail_attrs:
            return default
        value = self._f.read_attribute(name)
        if isinstance(value, (list, tuple)):          # string attribute(s)
            return value[0] if len(value) == 1 else list(value)
        value = np.asarray(value)
        return value.item() if value.size == 1 else value

    # ------------------------------------------------------------------ metadata
    def _read_global_attrs(self):
        return {name[len(GLOBAL_PREFIX):]: self._attr(name)
                for name in sorted(self._avail_attrs) if name.startswith(GLOBAL_PREFIX)}

    def _read_variables(self):
        names = set()
        for name in self._avail_vars:          # variables with data
            rest = name[len(VAR_PREFIX):]
            if name.startswith(VAR_PREFIX) and "/" not in rest:
                names.add(rest)
        for name in self._avail_attrs:         # also variables defined but never written
            if name.startswith(VAR_PREFIX) and name.endswith("/def/ndims"):
                names.add(name[len(VAR_PREFIX):-len("/def/ndims")])

        variables = {}
        for v in sorted(names):
            base = VAR_PREFIX + v
            ndims = int(self._attr(base + "/def/ndims", 0))
            dims = self._attr(base + "/def/dims", []) if ndims > 0 else []
            dims = [dims] if isinstance(dims, str) else list(dims)
            attrs = {}
            for name in sorted(self._avail_attrs):
                rest = name[len(base) + 1:]
                if name.startswith(base + "/") and "/" not in rest:
                    attrs[rest] = self._attr(name)
            variables[v] = SpioVariable(v, int(self._attr(base + "/def/nctype", 6)), dims,
                                        self._attr(base + "/def/ncop", "darray"), attrs)
        return variables

    def _read_block_list(self):
        """Writer ranks in each block group (one group per ADIOS writer)."""
        name = "/__pio__/info/block_list"
        if not self._has_var(name):
            return [[0]]
        step = self._first_step_with_blocks(name) or 0
        return [[int(w) for w in block] for block in self._read_step_blocks(name, step)]

    def _read_dims(self):
        dims, unlimited = {}, set()
        for name in sorted(self._avail_vars):
            if name.startswith(DIM_PREFIX):
                dname = name[len(DIM_PREFIX):]
                dims[dname] = int(self._read_value(name))
                if dims[dname] == PIO_UNLIMITED:
                    unlimited.add(dname)
        # An unlimited dimension's length is the number of records written
        if unlimited:
            nrec = 0
            for var in self.variables.values():
                if var.dims and var.dims[0] in unlimited:
                    nrec = max(nrec, self._num_records(var))
            for dname in unlimited:
                dims[dname] = nrec
        return unlimited, dims

    def _num_records(self, var):
        if var.op == "darray":
            frames = [self._read_step_concat(TRACK_PREFIX + "frame_id/" + var.name, s)
                      for s in range(self._nsteps)]
            frames = np.concatenate(frames) if frames else np.array([])
            return int(frames.max()) + 1 if frames.size and frames.max() >= 0 else 0
        nrec = 0
        for _, start, count, _ in self._put_var_blocks(var):
            if start is not None and count is not None:
                nrec = max(nrec, int(start[0] + count[0]))
            else:
                nrec += 1
        return nrec

    # ------------------------------------------------------------------ reading data
    def read(self, name):
        """Return variable `name` as a numpy array with its NetCDF shape."""
        var = self.variables[name]
        data = self._read_darray(var) if var.op == "darray" else self._read_put_var(var)
        if var.nctype == 2:                       # NC_CHAR
            return np.asarray(data).astype(np.uint8).view("S1")
        return np.asarray(data).astype(var.dtype, copy=False)

    def _fill_value(self, var, fill=None):
        if fill is not None:
            return fill
        if "_FillValue" in var.attrs:
            return np.asarray(var.attrs["_FillValue"]).ravel()[0]
        return NC_TYPES.get(var.nctype, (None, 0))[1] if var.nctype != 2 else 0

    def _decomposition(self, decomp_id):
        """The 1-based global index map of a decomposition, in block-group order,
        and the global shape it maps into (see ProcessOneDecomposition)."""
        if decomp_id in self._decomps:
            return self._decomps[decomp_id]
        name = DECOMP_PREFIX + str(decomp_id)
        step = self._first_step_with_blocks(name)
        if step is None:
            raise KeyError(f"Decomposition {name} not found")

        # Which writers have a block of this decomposition
        counts = self._read_step_blocks(TRACK_PREFIX + "num_decomp_block_writers/" + str(decomp_id), step)
        has_block = set()
        for g, group in enumerate(self._block_list):
            if g < len(counts) and counts[g].size:
                has_block.update(group[:int(counts[g][0])])
        # Blocks are numbered in writer order
        block_of = {w: i for i, w in enumerate(sorted(has_block))}

        pieces = [self._read_block(name, step, block_of[w])
                  for group in self._block_list for w in group if w in block_of]
        dmap = np.concatenate(pieces).astype(np.int64) if pieces else np.array([], dtype=np.int64)
        shape = tuple(int(d) for d in np.atleast_1d(self._attr(name + "/dimlen")))
        self._decomps[decomp_id] = (dmap, shape)
        return self._decomps[decomp_id]

    def _read_darray(self, var):
        """Distributed arrays written with PIOc_write_darray
        (see adios2_ConvertVariableDarray)."""
        vname = VAR_PREFIX + var.name
        records, nonrec, shape = {}, None, None
        for step in range(self._nsteps):
            frames = self._read_step_concat(TRACK_PREFIX + "frame_id/" + var.name, step)
            if frames.size == 0:
                continue
            decomp_ids = self._read_step_concat(TRACK_PREFIX + "decomp_id/" + var.name, step)
            fill_name = TRACK_PREFIX + "fillval_id/" + var.name
            fills = self._read_step_concat(fill_name, step) if self._has_var(fill_name) else []
            nwrites = frames.size

            # Writes present for each (writer, write index) in this step
            writer_counts = self._read_step_blocks(TRACK_PREFIX + "num_data_block_writers/" + var.name, step)
            ngroups = len(self._block_list)
            per_group = len(writer_counts) // ngroups
            if per_group * ngroups != len(writer_counts):
                raise ValueError(f"{var.name}: unexpected number of writer-count blocks in step {step}")
            present = set()
            b_idx = 0
            for g, group in enumerate(self._block_list):
                t = 0
                for _ in range(per_group):
                    for nwriters in writer_counts[b_idx]:
                        for k in range(int(nwriters)):
                            present.add((group[k], t))
                        t += 1
                    b_idx += 1
            # Data blocks are numbered writer by writer, then write by write
            block_of = {}
            for w in range(self._num_writers):
                for t in range(nwrites):
                    if (w, t) in present:
                        block_of[(w, t)] = len(block_of)

            fill_idx = 0
            for t in range(nwrites):
                decomp_id, frame, fill = int(decomp_ids[t]), int(frames[t]), None
                if decomp_id > 0:                 # positive id: a fill value was recorded
                    fill = fills[fill_idx]
                    fill_idx += 1
                else:
                    decomp_id = -decomp_id
                dmap, dshape = self._decomposition(decomp_id)
                shape = shape or dshape

                pieces = [self._read_block(vname, step, block_of[(w, t)])
                          for group in self._block_list for w in group if (w, t) in block_of]
                data = np.concatenate(pieces) if pieces else np.array([])
                n = min(dmap.size, data.size)
                idx, vals = dmap[:n], data[:n]
                valid = idx > 0                   # 0 = padding in short maps

                target = records.get(frame) if frame >= 0 else nonrec
                if target is None:
                    target = np.full(int(np.prod(dshape)), self._fill_value(var, fill),
                                     dtype=data.dtype if data.size else var.dtype)
                target[idx[valid] - 1] = vals[valid]
                if frame >= 0:
                    records[frame] = target
                else:
                    nonrec = target

        if shape is None:                         # defined but never written
            return np.array([], dtype=var.dtype)
        if not records:
            return nonrec.reshape(shape)
        nrec = max(records) + 1
        out = np.full((nrec, int(np.prod(shape))), self._fill_value(var),
                      dtype=next(iter(records.values())).dtype)
        for frame, flat in records.items():
            out[frame] = flat
        return out.reshape((nrec,) + shape)

    def _put_var_blocks(self, var):
        """(step, start, count, payload) of each block of a put_var variable.
        Each block holds start[ndims] and count[ndims] as int64, then the data
        (see adios2_ConvertVariablePutVar); a negative first entry means NULL."""
        vname = VAR_PREFIX + var.name
        if not self._has_var(vname):
            return
        ndims = len(var.dims)
        for step in range(self._nsteps):
            for raw in self._read_step_blocks(vname, step):
                if ndims == 0:
                    yield step, None, None, raw
                    continue
                buf = np.ascontiguousarray(raw).view(np.uint8)
                hdr = np.frombuffer(buf[:16 * ndims].tobytes(), dtype=np.int64)
                start = hdr[:ndims] if hdr[0] >= 0 else None
                count = hdr[ndims:] if hdr[ndims] >= 0 else None
                payload = np.frombuffer(buf[16 * ndims:].tobytes(), dtype=raw.dtype)
                yield step, start, count, payload

    def _read_put_var(self, var):
        blocks = list(self._put_var_blocks(var))
        if not var.dims:                          # scalar: the last value written
            return blocks[-1][3][:1].reshape(()) if blocks else np.array([], dtype=var.dtype)
        shape = [self.dims[d] for d in var.dims]
        out = np.full(shape, self._fill_value(var), dtype=blocks[0][3].dtype if blocks else var.dtype)
        record = 0
        for _, start, count, payload in blocks:
            if count is None:                     # whole variable, or one whole record
                count = np.array(shape)
                if var.dims[0] in self.unlimited:
                    count[0] = 1
            if start is None:
                start = np.zeros(len(shape), dtype=np.int64)
                if var.dims[0] in self.unlimited:
                    start[0] = record
            record += 1
            region = tuple(slice(int(s), int(s + c)) for s, c in zip(start, count))
            out[region] = payload[:int(np.prod(count))].reshape([int(c) for c in count])
        return out


# ---------------------------------------------------------------------- command line
def _fmt(value):
    if isinstance(value, str):
        return '"' + value + '"'
    if isinstance(value, list):
        return ", ".join('"' + str(v) + '"' for v in value)
    return ", ".join(str(v) for v in np.atleast_1d(value))


def print_header(f, out=sys.stdout):
    print(f"spio {f.path} {{", file=out)
    print("dimensions:", file=out)
    for name, length in f.dims.items():
        if name in f.unlimited:
            print(f"\t{name} = UNLIMITED ; // ({length} currently)", file=out)
        else:
            print(f"\t{name} = {length} ;", file=out)
    print("variables:", file=out)
    for var in f.variables.values():
        tname = NC_TYPES.get(var.nctype, (None, None, f"nctype{var.nctype}"))[2]
        dims = f"({', '.join(var.dims)})" if var.dims else ""
        print(f"\t{tname} {var.name}{dims} ;", file=out)
        for aname, value in var.attrs.items():
            print(f"\t\t{var.name}:{aname} = {_fmt(value)} ;", file=out)
    if f.attrs:
        print("\n// global attributes:", file=out)
        for aname, value in f.attrs.items():
            print(f"\t\t:{aname} = {_fmt(value)} ;", file=out)
    print("}", file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Read an ADIOS2 BP file written by SCORPIO.")
    parser.add_argument("path", help="file name given to SCORPIO (e.g. out.nc) or its .bp directory")
    parser.add_argument("-v", "--variables", nargs="+", default=[], help="variables whose values to print")
    args = parser.parse_args(argv)

    with SpioFile(args.path) as f:
        print_header(f)
        np.set_printoptions(threshold=2000, linewidth=120)
        for name in args.variables:
            print(f"\n{name} =\n{f.read(name)}")


if __name__ == "__main__":
    main()
