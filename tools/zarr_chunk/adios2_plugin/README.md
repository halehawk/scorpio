# ZarrWrite: an ADIOS2 engine plugin that writes Zarr

`ZarrWrite` is an ADIOS2 engine, loaded at run time as a plugin, that writes
an IO's variables as a [Zarr v3](https://zarr-specs.readthedocs.io/en/latest/v3/core/index.html)
store that zarr-python and xarray can open. An application already using
ADIOS2 switches to Zarr output by changing its XML config. Its code does not
change.

- [Requirements](#requirements)
- [Build](#build)
- [Using it from XML](#using-it-from-xml)
- [Parameters](#parameters)
- [What gets written](#what-gets-written)
- [Serial or MPI build](#serial-or-mpi-build)
- [Using it from code instead of XML](#using-it-from-code-instead-of-xml)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)

## Requirements

| | |
|---|---|
| ADIOS2 | **2.12 or newer**, built as shared libraries (the default). The plugin interface changed in 2.12, so 2.11 and older will not load it. |
| Compiler | C++17 |
| CMake | 3.12 or newer |
| MPI | only for the MPI build of the plugin. ADIOS2 must then be built with MPI too. |
| Python (tests only) | `zarr>=3`, `numpy`, and the `adios2` module of the same ADIOS2 install. `xarray` is optional. |

## Build

### 1. Get ADIOS2

**With conda** (simplest):

```bash
conda create -n zarr_env -c conda-forge python=3.12 cmake cxx-compiler \
      mpich mpich-mpicxx "adios2=*=mpi_mpich_*" "zarr>=3" numpy xarray
conda activate zarr_env
```

The `mpi_mpich_*` build contains both the serial and the MPI ADIOS2
libraries, so it works for both plugin builds.

**From source:** no special option is needed, because the plugin engine is
always part of ADIOS2.

```bash
git clone https://github.com/ornladios/ADIOS2.git
cmake -S ADIOS2 -B adios2-build -DCMAKE_INSTALL_PREFIX=$HOME/opt/adios2 \
      -DADIOS2_USE_MPI=ON            # only needed for the MPI plugin build
cmake --build adios2-build -j && cmake --install adios2-build
```

Keep the default `BUILD_SHARED_LIBS=ON`. The plugin is `dlopen`ed and must
share ADIOS2's libraries with the application.

### 2. Build the plugin

From `tools/zarr_chunk/adios2_plugin`:

```bash
# serial plugin (default)
cmake -S . -B build -DCMAKE_PREFIX_PATH=$CONDA_PREFIX      # or $HOME/opt/adios2
cmake --build build

# MPI plugin
CXX=mpicxx cmake -S . -B build_mpi -DCMAKE_PREFIX_PATH=$CONDA_PREFIX -DZARR_PLUGIN_USE_MPI=ON
cmake --build build_mpi
```

This produces:

| target | file |
|---|---|
| the plugin | `build/libadios2_zarr_plugin.so` (`.dylib` on macOS) |
| `zarr_plugin_writer` | small example writer |
| `zarr_backend_test` | test application (see [Testing](#3-test-it)) |

To install the plugin somewhere permanent:

```bash
cmake --install build --prefix $HOME/opt/zarr_plugin    # -> $HOME/opt/zarr_plugin/lib/
```

### 3. Test it

```bash
cd build && ctest --output-on-failure
```

The test runs one ADIOS2 application twice, once with
`test/zarr_backend.xml` (this plugin) and once with `test/bp5_backend.xml`
(BP5). It then checks that every step of every variable in the Zarr store
equals the BP5 file. It also checks the chunk shapes, dimension names, fill
values and attributes. The serial build runs one test; the MPI build runs it
on 1, 3 and 4 ranks.

## Using it from XML

### Tell ADIOS2 where the plugin is

ADIOS2 finds the library from `PluginLibrary` in one of these ways (on Linux
it adds the `lib` prefix and the `.so` suffix itself):

1. **`ADIOS2_PLUGIN_PATH`** (recommended): a `:`-separated list of
   directories.

   ```bash
   export ADIOS2_PLUGIN_PATH=$HOME/opt/zarr_plugin/lib
   ```

2. **The normal library search path** (`LD_LIBRARY_PATH`, or
   `DYLD_LIBRARY_PATH` on macOS), when `ADIOS2_PLUGIN_PATH` is not set.

3. **A full path** in `PluginLibrary`, e.g.
   `/home/me/opt/zarr_plugin/lib/libadios2_zarr_plugin.so`. This only works
   when `ADIOS2_PLUGIN_PATH` is not set, because ADIOS2 otherwise prepends
   those directories to the name.

### The XML

Select the `Plugin` engine for the IO, and name the plugin and its library:

```xml
<?xml version="1.0"?>
<adios-config>
  <io name="Output">
    <engine type="Plugin">
      <!-- required -->
      <parameter key="PluginName"    value="ZarrWrite"/>
      <parameter key="PluginLibrary" value="adios2_zarr_plugin"/>
      <parameter key="FileName"      value="output.zarr"/>
    </engine>
  </io>
</adios-config>
```

> **`FileName` is required, and it sets the output directory.** ADIOS2 2.12
> does not pass the name given to `engine = io.Open(name, ...)` on to plugins,
> so the plugin cannot see it. Normally you set `FileName` to the same name
> the application opens.

All `<parameter>` elements must be **inside `<engine>`**. ADIOS2 silently
ignores parameters placed directly under `<io>`.

### A full example

From `test/zarr_backend.xml`, for an application that writes
`T(lev, lat, lon)`, `ps(lat, lon)`, `mask(lat, lon)`, `flux(lon)` and a
scalar `time`:

```xml
<?xml version="1.0"?>
<adios-config>
  <io name="Output">
    <engine type="Plugin">
      <parameter key="PluginName"      value="ZarrWrite"/>
      <parameter key="PluginLibrary"   value="adios2_zarr_plugin"/>
      <parameter key="FileName"        value="backend_test.zarr"/>

      <!-- chunk shape and dimension names per variable -->
      <parameter key="ChunkShape.T"    value="2,3,4"/>
      <parameter key="DimNames.T"      value="lev,lat,lon"/>
      <parameter key="ChunkShape.ps"   value="4,5"/>
      <parameter key="DimNames.ps"     value="lat,lon"/>
      <parameter key="DimNames.mask"   value="lat,lon"/>
      <parameter key="FillValue.mask"  value="255"/>
      <parameter key="ChunkShape.flux" value="3"/>
      <parameter key="DimNames.flux"   value="lon"/>

      <!-- default fill value for every other variable -->
      <parameter key="FillValue"       value="nan"/>
    </engine>
  </io>
</adios-config>
```

The application loads the XML the usual way:

```cpp
adios2::ADIOS adios("zarr_backend.xml", MPI_COMM_WORLD);   // or adios2::ADIOS adios("zarr_backend.xml");
adios2::IO io = adios.DeclareIO("Output");                  // must match <io name="...">
// ... DefineVariable / DefineAttribute ...
adios2::Engine w = io.Open("backend_test.zarr", adios2::Mode::Write);
for (...) { w.BeginStep(); w.Put(...); w.EndStep(); }
w.Close();
```

Switching the same application back to BP5 only takes a different XML:

```xml
<io name="Output">
  <engine type="BP5"/>
</io>
```

## Parameters

Parameter names are case-insensitive, and that includes the `<var>` part.
`<var>` is the full ADIOS2 variable name, e.g. `ChunkShape.atm/u`.

| parameter | default | meaning |
|---|---|---|
| `PluginName` | (required) | `ZarrWrite` |
| `PluginLibrary` | (required) | `adios2_zarr_plugin`, or a full path (see above) |
| `FileName` | (required) | Output directory, e.g. `out.zarr`. `DirName` is accepted as an alias. |
| `ChunkShape.<var>` | the whole array | Chunk shape of `<var>`, one size per dimension, **not** counting the step dimension, e.g. `2,3,4`. A wrong number of sizes is an error. |
| `ChunkShape` | none | Default chunk shape for every array with that many dimensions. Arrays with a different number of dimensions use the whole array. |
| `FillValue.<var>` | `FillValue` | Fill value of `<var>`. `nan`, `inf` and `-inf` work for floating-point types. |
| `FillValue` | `0` | Default fill value. For integer variables a non-integer value such as `nan` becomes `0`, so set `FillValue.<var>` for those. |
| `DimNames.<var>` | `<name>_dim0, ...` | Dimension names, e.g. `lat,lon`, not counting `step`. A wrong number of names falls back to the defaults. |
| `WriteEmptyChunks` | `false` | When `false` (zarr-python's default), chunks containing only the fill value are not written. |

**Choosing a chunk shape:** with one chunk per step (the default), a single
process assembles the whole array for every step. For large arrays, pick
chunks of roughly 1–16 MB. With the MPI build, chunks aligned to the data
decomposition need no communication (see below).

## What gets written

For `FileName=output.zarr`:

```
output.zarr/
  zarr.json                 group; attributes = ADIOS2 global attributes
  T/
    zarr.json               array metadata (shape, chunks, dims, fill, attributes)
    c/<step>/<i>/<j>/<k>    one file per chunk
  atm/                      a variable named "atm/u" becomes a nested group
    zarr.json
    u/...
```

- **A step dimension is added.** Every global array or global value becomes
  a Zarr array with a leading `step` dimension: ADIOS2 step `s` is index `s`.
  `T` with shape `(4, 6, 10)` written for 5 steps becomes shape
  `(5, 4, 6, 10)` with chunks `(1, *ChunkShape.T)`. A global value becomes
  shape `(nsteps,)`.
- **Missing data is the fill value.** A step in which a variable was not
  written, and any part of an array that no block covered, reads back as
  the fill value.
- **Attributes:** an ADIOS2 attribute named `<var>/<name>` (e.g. `T/units`)
  becomes attribute `<name>` of array `<var>`. Any other attribute becomes a
  group attribute. String and numeric values, single or arrays, are
  supported.
- **Updated during the run:** the metadata (`zarr.json`) is rewritten at
  every `EndStep`, so the store can be read while the run is still going.
- **No compression:** chunks are stored raw (codec `bytes`, little-endian).
  The layout is byte for byte what zarr-python writes with
  `compressors=None`.

Reading it:

```python
import zarr, xarray as xr
g = zarr.open_group("output.zarr", mode="r")
T = g["T"][...]                       # (step, lev, lat, lon)
ds = xr.open_zarr("output.zarr", consolidated=False)
```

## Serial or MPI build

| | serial (default) | MPI (`-DZARR_PLUGIN_USE_MPI=ON`) |
|---|---|---|
| Needs MPI | no; links only `libadios2_cxx` | yes; ADIOS2 built with MPI |
| Who builds a chunk | the process that Put the data | one owner rank per chunk, which receives the other ranks' pieces in one `MPI_Alltoallv` |
| Several writer processes | correct only if no chunk spans two processes | always correct |
| Communicator | none | `MPI_COMM_WORLD` (see below) |

**When to use the serial build:**

- **A single writer process.** This is always correct, with any number of
  `Put`s per variable.
- **Several processes whose data blocks line up with chunk boundaries.**
  Example: each rank writes an x slab of width 4 and `ChunkShape` uses 4
  along x. Each process then writes only its own chunks. Every process also
  writes the same `zarr.json`.

Otherwise, use the MPI build.

**The MPI build always uses `MPI_COMM_WORLD`.** ADIOS2 2.12 does not hand
plugins the communicator passed to `adios2::ADIOS`: its plugin loader is
compiled into the serial core library. So **every rank of `MPI_COMM_WORLD`
must open the engine.** An application that writes from a sub-communicator
cannot use the MPI build yet.

In the MPI build, the owner of a chunk is the rank holding the chunk's
first element. Data moves between ranks only for chunks that span ranks.

## Using it from code instead of XML

```cpp
io.SetEngine("Plugin");
io.SetParameters({{"PluginName", "ZarrWrite"},
                  {"PluginLibrary", "adios2_zarr_plugin"},
                  {"FileName", "output.zarr"},
                  {"ChunkShape.T", "2,3,4"},
                  {"DimNames.T", "lev,lat,lon"}});
adios2::Engine w = io.Open("output.zarr", adios2::Mode::Write);
```

## Limitations

- **Write only.** `Mode::Read` and `Mode::Append` are rejected. Read the
  output with zarr-python, xarray, or any Zarr v3 reader.
- **Only global data is written.** Global arrays and global values are
  supported. Local arrays, local values and joined arrays have no global
  shape and are skipped with a warning.
- **Supported types:** `int8`–`int64`, `uint8`–`uint64`, `float` and
  `double`. `std::string`, `char`, `long double` and complex variables are
  skipped with a warning.
- **Fixed shape:** an array's shape is taken when it is first written, and
  must not change in later steps.
- **Same variables everywhere:** all writer processes must define the same
  variables. In the MPI build this is checked, and the run stops if it
  isn't.
- **No compression yet.**

## Troubleshooting

| symptom | cause and fix |
|---|---|
| `Couldn't find engine plugin named ZarrWrite`, preceded by the warning `The plugin adios2_zarr_plugin could not be loaded. Double check ADIOS2_PLUGIN_PATH ...` | The library was not found. Set `ADIOS2_PLUGIN_PATH` to the directory containing `libadios2_zarr_plugin.so`, or see [Tell ADIOS2 where the plugin is](#tell-adios2-where-the-plugin-is). To see where ADIOS2 looks, add `<parameter key="verbose" value="5"/>`. |
| `ZarrWritePlugin: set the FileName parameter to the output directory` | `FileName` is missing, or sits outside `<engine>`. |
| Parameters seem to be ignored | They are placed under `<io>` instead of inside `<engine>`, or the `<io name>` does not match `DeclareIO(...)`. |
| `ZarrWritePlugin: ChunkShape.<var> has N sizes, <var> has M dimensions` | Give one size per dimension, without the step dimension. |
| `ZarrWritePlugin: ranks define different variables` | (MPI build) Some ranks define variables that others don't. Define every variable on every rank, even where a rank `Put`s nothing. |
| MPI hangs, or `Invalid communicator` | (MPI build) Not every rank of `MPI_COMM_WORLD` opened the engine; see [Serial or MPI build](#serial-or-mpi-build). An `Invalid communicator` error can also mean the plugin was built against a different ADIOS2 or MPI than the application. |
| Chunks partly filled with the fill value, data from other ranks lost | (Serial build with several processes) A chunk spans processes. Align `ChunkShape` with the decomposition or use the MPI build. |
| `ZarrWritePlugin: skipping <var> (...)` | The variable is a local array or has an unsupported type; see [Limitations](#limitations). |
