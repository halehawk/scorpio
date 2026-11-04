# zarr_chunk

Chunking of N-D arrays the way zarr-python does it (Zarr v3, regular chunk
grid, edge chunks padded with the fill value, C order, `c/i/j` keys, no
compression), and an ADIOS2 engine plugin that writes through it.

| file | what |
|---|---|
| `zarr_chunk.hpp` | serial, header only: `chunk_array`, `chunk_blocks`, `write_zarr_v3` |
| `zarr_chunk_mpi.hpp` | MPI: `chunk_blocks_mpi` / `chunk_array_mpi` assemble each chunk on one owner rank (one `MPI_Alltoallv`), `write_zarr_v3_mpi` |
| `adios2_plugin/` | `ZarrWrite` ADIOS2 engine plugin (serial by default, `-DZARR_PLUGIN_USE_MPI=ON` for MPI) |

## Tests

All compare against zarr-python 3 (values and chunk files byte for byte).

```bash
# chunkers
c++ -std=c++17 zarr_chunk_test.cpp -o zarr_chunk_test && ./zarr_chunk_test out && python check_zarr_chunk.py out
mpicxx -std=c++17 zarr_chunk_mpi_test.cpp -o zarr_chunk_mpi_test
mpiexec -n 3 ./zarr_chunk_mpi_test out_mpi && python check_zarr_chunk.py out_mpi

# plugin: CTest runs a plain ADIOS2 application with the Zarr plugin and with
# BP5 (chosen by XML) and compares the two outputs
cmake -S adios2_plugin -B build [-DZARR_PLUGIN_USE_MPI=ON] && cmake --build build
cd build && ctest --output-on-failure
```

Python needs `zarr>=3` and the `adios2` module of the same ADIOS2 install.

## ADIOS2 plugin notes (ADIOS2 2.12)

- ADIOS2 passes the plugin name, not the name given to `Open()`, as the
  engine name, so the output directory is set with the `FileName` parameter.
- ADIOS2 never passes the communicator to plugins (the plugin loader is in
  the serial core library), so the MPI build uses `MPI_COMM_WORLD`.
- The serial plugin builds chunks only from blocks Put by its own process:
  exact for one writer process; with several, only if no chunk spans
  processes.
