// Writes a few arrays with zarr_chunk.hpp into OUTDIR (argv[1]); then
// check_zarr_chunk.py compares them with what zarr-python produces.
//   c++ -std=c++17 -O2 zarr_chunk_test.cpp -o zarr_chunk_test
//   ./zarr_chunk_test out && python check_zarr_chunk.py out

#include "zarr_chunk.hpp"

#include <cmath>
#include <iostream>

int main(int argc, char* argv[])
{
    const std::string outdir = argc > 1 ? argv[1] : "zarr_chunk_out";

    // 3-D (time, y, x) like pres in pio_sst_multivar: edge chunks in y and x.
    {
        std::vector<size_t> shape = {10, 3, 8}, chunks = {4, 2, 3};
        std::vector<int32_t> a(10 * 3 * 8);
        for (size_t f = 0; f < 10; f++)
            for (size_t y = 0; y < 3; y++)
                for (size_t x = 0; x < 8; x++)
                    a[(f * 3 + y) * 8 + x] = int32_t(f * 10000 + y * 100 + x);
        zarr_chunk::write_zarr_v3(outdir + "/pres.zarr", a.data(), shape, chunks,
                                  int32_t(-1), {"time", "y", "x"});
    }

    // 2-D double with NaN fill; some chunks are all fill and are skipped.
    {
        std::vector<size_t> shape = {7, 5}, chunks = {3, 2};
        std::vector<double> a(7 * 5, NAN);
        for (size_t i = 0; i < 3; i++)
            for (size_t j = 0; j < 5; j++)
                a[i * 5 + j] = 0.5 * double(i * 5 + j);
        zarr_chunk::write_zarr_v3(outdir + "/sparse.zarr", a.data(), shape, chunks,
                                  double(NAN));
    }

    // 1-D float, chunk larger than the array.
    {
        std::vector<size_t> shape = {5}, chunks = {8};
        std::vector<float> a = {1, 2, 3, 4, 5};
        zarr_chunk::write_zarr_v3(outdir + "/small.zarr", a.data(), shape, chunks, 0.0f);
    }

    // Chunk keys in v2 encoding, printed for the check script.
    {
        std::vector<size_t> shape = {4, 4}, chunks = {3, 3};
        std::vector<uint8_t> a(16, 1);
        for (const auto& c : zarr_chunk::chunk_array(a.data(), shape, chunks, uint8_t(0), false,
                                                     zarr_chunk::KeyEncoding::V2))
            std::cout << "v2 key " << c.key << " " << c.bytes.size() << " bytes\n";
    }
    return 0;
}
