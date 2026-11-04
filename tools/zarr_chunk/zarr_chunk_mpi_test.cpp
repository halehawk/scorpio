// Writes the same arrays as zarr_chunk_test.cpp, but distributed over the
// MPI ranks in several ways, with zarr_chunk_mpi.hpp. check_zarr_chunk.py
// then compares each store with zarr-python, byte for byte.
//   mpicxx -std=c++17 -O2 zarr_chunk_mpi_test.cpp -o zarr_chunk_mpi_test
//   mpiexec -n 3 ./zarr_chunk_mpi_test out && python check_zarr_chunk.py out

#include "zarr_chunk_mpi.hpp"

#include <cmath>
#include <iostream>

using zarr_chunk::Box;

// Split [0, n) into nprocs nearly equal pieces; piece `r`.
static void split(size_t n, int nprocs, int r, size_t& start, size_t& count)
{
    size_t base = n / nprocs, extra = n % nprocs;
    start = r * base + std::min<size_t>(r, extra);
    count = base + (static_cast<size_t>(r) < extra ? 1 : 0);
}

// Copy box b out of a full C-order array of shape `shape`.
template <typename T>
static std::vector<T> take(const std::vector<T>& full, const std::vector<size_t>& shape,
                           const Box& b)
{
    size_t n = 1;
    for (size_t c : b.count)
        n *= c;
    std::vector<T> out(n);
    if (n == 0)
        return out;
    Box whole{std::vector<size_t>(shape.size(), 0), shape};
    zarr_chunk::detail::copy_region(full.data(), whole, out.data(), b, b);
    return out;
}

int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    const std::string outdir = argc > 1 ? argv[1] : "zarr_chunk_mpi_out";

    try {
        // pres (time, y, x) = (10, 3, 8), chunks (4, 2, 3)
        std::vector<size_t> shape = {10, 3, 8}, chunks = {4, 2, 3};
        std::vector<int32_t> pres(10 * 3 * 8);
        for (size_t f = 0; f < 10; f++)
            for (size_t y = 0; y < 3; y++)
                for (size_t x = 0; x < 8; x++)
                    pres[(f * 3 + y) * 8 + x] = int32_t(f * 10000 + y * 100 + x);

        // 1) split along x, not aligned to the chunks: pieces cross ranks
        Box b{{0, 0, 0}, {10, 3, 0}};
        split(8, nprocs, rank, b.start[2], b.count[2]);
        auto local = take(pres, shape, b);
        zarr_chunk::write_zarr_v3_mpi(MPI_COMM_WORLD, outdir + "/pres.zarr", local.data(), b,
                                      shape, chunks, int32_t(-1), {"time", "y", "x"});

        // 2) split along time, aligned to the 4-frame chunks (no data moves)
        Box t{{0, 0, 0}, {0, 3, 8}};
        size_t nblocks = (10 + 3) / 4, b0, nb;
        split(nblocks, nprocs, rank, b0, nb);
        t.start[0] = std::min<size_t>(b0 * 4, 10);
        t.count[0] = std::min<size_t>((b0 + nb) * 4, 10) - t.start[0];
        local = take(pres, shape, t);
        zarr_chunk::write_zarr_v3_mpi(MPI_COMM_WORLD, outdir + "/pres_t.zarr", local.data(), t,
                                      shape, chunks, int32_t(-1), {"time", "y", "x"});

        // 3) only the last rank holds data; the others pass empty boxes
        Box all{{0, 0, 0}, shape};
        Box none{{0, 0, 0}, {0, 0, 0}};
        const Box& mine = rank == nprocs - 1 ? all : none;
        local = take(pres, shape, mine);
        zarr_chunk::write_zarr_v3_mpi(MPI_COMM_WORLD, outdir + "/pres_one.zarr", local.data(),
                                      mine, shape, chunks, int32_t(-1));

        // 4) two blocks per rank: x split into 2*nprocs slabs, rank r holds
        //    slabs r and r + nprocs
        std::vector<Box> two(2, Box{{0, 0, 0}, {10, 3, 0}});
        split(8, 2 * nprocs, rank, two[0].start[2], two[0].count[2]);
        split(8, 2 * nprocs, rank + nprocs, two[1].start[2], two[1].count[2]);
        auto l0 = take(pres, shape, two[0]), l1 = take(pres, shape, two[1]);
        {
            const std::string path = outdir + "/pres_multi.zarr";
            if (rank == 0) {
                std::filesystem::create_directories(path);
                std::ofstream(path + "/zarr.json")
                    << zarr_chunk::zarr_v3_metadata(shape, chunks, int32_t(-1));
            }
            MPI_Barrier(MPI_COMM_WORLD);
            for (const auto& c : zarr_chunk::chunk_blocks_mpi<int32_t>(
                     MPI_COMM_WORLD, {l0.data(), l1.data()}, two, shape, chunks, int32_t(-1)))
                zarr_chunk::write_chunk_file(path, c);
            MPI_Barrier(MPI_COMM_WORLD);
        }

        // sparse (7, 5) double, NaN fill: rows 0-2 hold data, split by rows
        // among the ranks; rows 3-6 are held by nobody and stay fill.
        std::vector<size_t> sshape = {7, 5}, schunks = {3, 2};
        std::vector<double> sparse(7 * 5, NAN);
        for (size_t i = 0; i < 15; i++)
            sparse[i] = 0.5 * double(i);
        Box s{{0, 0}, {0, 5}};
        split(3, nprocs, rank, s.start[0], s.count[0]);
        auto slocal = take(sparse, sshape, s);
        zarr_chunk::write_zarr_v3_mpi(MPI_COMM_WORLD, outdir + "/sparse.zarr", slocal.data(), s,
                                      sshape, schunks, double(NAN));

        // small (5,) float, one chunk of 8 assembled from all ranks
        std::vector<size_t> mshape = {5}, mchunks = {8};
        std::vector<float> small = {1, 2, 3, 4, 5};
        Box m{{0}, {0}};
        split(5, nprocs, rank, m.start[0], m.count[0]);
        auto mlocal = take(small, mshape, m);
        zarr_chunk::write_zarr_v3_mpi(MPI_COMM_WORLD, outdir + "/small.zarr", mlocal.data(), m,
                                      mshape, mchunks, 0.0f);
    }
    catch (std::exception& e) {
        std::cerr << "rank " << rank << ": " << e.what() << "\n";
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    if (rank == 0)
        std::cout << "wrote stores in " << outdir << " with " << nprocs << " ranks\n";
    MPI_Finalize();
    return 0;
}
