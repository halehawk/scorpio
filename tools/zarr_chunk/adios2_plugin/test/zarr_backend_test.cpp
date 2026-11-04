// A plain ADIOS2 application: the output backend is chosen only by the XML
// config (argv[1]), e.g. zarr_backend.xml (Plugin -> ZarrWrite) or
// bp5_backend.xml. compare_zarr_bp.py checks that both hold the same data.
//   zarr_backend_test <config.xml> <output name>
//
// Data, over NSTEPS steps, split into parts as if written by several ranks
// (MPI: one part per rank; serial: one process Puts all NPARTS parts):
//   T     (lev, lat, lon) double, split along lon, Put sync
//   ps    (lat, lon)      float,  split along lat, Put deferred
//   time  global value    double, one per step
//   mask  (lat, lon)      uint8,  only in step 0
//   flux  (lon)           int64,  two blocks per part, Put deferred

#include <adios2.h>
#ifdef ZARR_PLUGIN_MPI
#include <mpi.h>
#endif

#include <algorithm>
#include <iostream>
#include <vector>

static const size_t NSTEPS = 5, NLEV = 4, NLAT = 6, NLON = 10, NPARTS_SERIAL = 3;

// Split [0, n) into nparts nearly equal pieces; piece `p`.
static void split(size_t n, int nparts, int p, size_t &start, size_t &count)
{
    size_t base = n / nparts, extra = n % nparts;
    start = p * base + std::min<size_t>(p, extra);
    count = base + (static_cast<size_t>(p) < extra ? 1 : 0);
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <config.xml> <output name>\n";
        return 2;
    }
    int rank = 0, nparts = NPARTS_SERIAL;
    std::vector<int> parts;
#ifdef ZARR_PLUGIN_MPI
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nparts);
    parts.push_back(rank);
#else
    for (int p = 0; p < nparts; p++)
        parts.push_back(p);
#endif

    int ret = 0;
    try
    {
#ifdef ZARR_PLUGIN_MPI
        adios2::ADIOS adios(argv[1], MPI_COMM_WORLD);
#else
        adios2::ADIOS adios(argv[1]);
#endif
        adios2::IO io = adios.DeclareIO("Output");

        auto T = io.DefineVariable<double>("T", {NLEV, NLAT, NLON}, {0, 0, 0}, {NLEV, NLAT, 1});
        auto ps = io.DefineVariable<float>("ps", {NLAT, NLON}, {0, 0}, {1, NLON});
        auto time = io.DefineVariable<double>("time");
        auto mask = io.DefineVariable<uint8_t>("mask", {NLAT, NLON}, {0, 0}, {1, NLON});
        auto flux = io.DefineVariable<int64_t>("flux", {NLON}, {0}, {1});

        io.DefineAttribute<std::string>("Conventions", "CF-1.8");
        io.DefineAttribute<std::string>("T/units", "K");
        io.DefineAttribute<std::string>("T/long_name", "air temperature");
        io.DefineAttribute<float>("ps/scale_factor", 0.5f);
        io.DefineAttribute<int32_t>("flux/valid_range", std::vector<int32_t>{-100, 100}.data(), 2);

        adios2::Engine w = io.Open(argv[2], adios2::Mode::Write);
        for (size_t s = 0; s < NSTEPS; s++)
        {
            w.BeginStep();
            std::vector<std::vector<float>> psbufs;  // deferred: keep until EndStep
            std::vector<std::vector<int64_t>> fbufs;
            for (int part : parts)
            {
                size_t x0, nx;
                split(NLON, nparts, part, x0, nx);
                if (nx > 0)
                {
                    std::vector<double> t(NLEV * NLAT * nx);
                    for (size_t k = 0; k < NLEV; k++)
                        for (size_t j = 0; j < NLAT; j++)
                            for (size_t i = 0; i < nx; i++)
                                t[(k * NLAT + j) * nx + i] =
                                    250.0 + s + 10.0 * k + 0.1 * j + 0.001 * (x0 + i);
                    T.SetSelection({{0, 0, x0}, {NLEV, NLAT, nx}});
                    w.Put(T, t.data(), adios2::Mode::Sync);
                }

                size_t y0, ny;
                split(NLAT, nparts, part, y0, ny);
                if (ny > 0)
                {
                    std::vector<float> &p = psbufs.emplace_back(ny * NLON);
                    for (size_t j = 0; j < ny; j++)
                        for (size_t i = 0; i < NLON; i++)
                            p[j * NLON + i] = 1000.0f + float(s) + 0.5f * float((y0 + j) * NLON + i);
                    ps.SetSelection({{y0, 0}, {ny, NLON}});
                    w.Put(ps, p.data());

                    if (s == 0)
                    {
                        std::vector<uint8_t> m(ny * NLON);
                        for (size_t j = 0; j < ny; j++)
                            for (size_t i = 0; i < NLON; i++)
                                m[j * NLON + i] = uint8_t(((y0 + j) + i) % 2);
                        mask.SetSelection({{y0, 0}, {ny, NLON}});
                        w.Put(mask, m.data(), adios2::Mode::Sync);
                    }
                }

                for (int b : {part, part + nparts})
                {
                    size_t f0, nf;
                    split(NLON, 2 * nparts, b, f0, nf);
                    if (nf == 0)
                        continue;
                    std::vector<int64_t> &f = fbufs.emplace_back(nf);
                    for (size_t i = 0; i < nf; i++)
                        f[i] = int64_t(s) * 1000 - int64_t(f0 + i);
                    flux.SetSelection({{f0}, {nf}});
                    w.Put(flux, f.data());
                }
            }
            if (rank == 0)
                w.Put(time, 3600.0 * double(s));
            w.EndStep();
        }
        w.Close();
    }
    catch (std::exception &e)
    {
        std::cerr << "rank " << rank << ": " << e.what() << "\n";
        ret = 1;
    }
    if (rank == 0 && ret == 0)
        std::cout << "wrote " << argv[2] << " with " << argv[1] << " (" << nparts << " parts)\n";
#ifdef ZARR_PLUGIN_MPI
    MPI_Finalize();
#endif
    return ret;
}
