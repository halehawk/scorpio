// Writes a few variables over several steps through the ZarrWrite plugin;
// check_zarr_plugin.py checks the result with zarr-python.
//   serial:  ADIOS2_PLUGIN_PATH=<build dir> ./zarr_plugin_writer out.zarr
//   MPI:     ADIOS2_PLUGIN_PATH=<build dir> mpiexec -n 3 ./zarr_plugin_writer out.zarr
// The data is split into "parts" as if written by several ranks: with MPI
// each rank Puts its part; serial, the one process Puts all NPARTS parts.

#include <adios2.h>
#ifdef ZARR_PLUGIN_MPI
#include <mpi.h>
#endif

#include <algorithm>
#include <iostream>
#include <vector>

// Split [0, n) into nparts nearly equal pieces; piece `p`.
static void split(size_t n, int nparts, int p, size_t &start, size_t &count)
{
    size_t base = n / nparts, extra = n % nparts;
    start = p * base + std::min<size_t>(p, extra);
    count = base + (static_cast<size_t>(p) < extra ? 1 : 0);
}

int main(int argc, char *argv[])
{
    int rank = 0, nparts = 3;
#ifdef ZARR_PLUGIN_MPI
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nparts);
    std::vector<int> parts{rank};
#else
    std::vector<int> parts{0, 1, 2};
#endif
    const std::string out = argc > 1 ? argv[1] : "plugin_out.zarr";
    const size_t NSTEPS = 4, NY = 3, NX = 8, NT = 10, UY = 6, UX = 5;

    int ret = 0;
    try
    {
#ifdef ZARR_PLUGIN_MPI
        adios2::ADIOS adios(MPI_COMM_WORLD);
#else
        adios2::ADIOS adios;
#endif
        adios2::IO io = adios.DeclareIO("zarr");
        io.SetEngine("Plugin");
        io.SetParameters({{"PluginName", "ZarrWrite"},
                          {"PluginLibrary", "adios2_zarr_plugin"},
                          {"FileName", out},
                          {"ChunkShape.pres", "2,3"},
                          {"FillValue.pres", "-1"},
                          {"DimNames.pres", "y,x"},
                          {"ChunkShape.temp", "4"},
                          {"FillValue", "nan"}});

        // pres (y, x) int: x split into parts, not aligned to the chunks
        auto pres = io.DefineVariable<int32_t>("pres", {NY, NX}, {0, 0}, {NY, 1});
        // temp (x) double: two blocks per part, Put deferred
        auto temp = io.DefineVariable<double>("temp", {NT}, {0}, {1});
        // p0: one value per step
        auto p0 = io.DefineVariable<double>("p0");
        // atm/u (y, x) float: rows split into parts
        auto u = io.DefineVariable<float>("atm/u", {UY, UX}, {0, 0}, {1, UX});
        // skipped by the plugin: a local array and a string
        auto loc = io.DefineVariable<int32_t>("loc", {}, {}, {2});
        auto label = io.DefineVariable<std::string>("label");

        io.DefineAttribute<std::string>("title", "ZarrWrite plugin test");
        io.DefineAttribute<std::string>("pres/units", "Pa");
        io.DefineAttribute<double>("temp/valid_range", std::vector<double>{0.0, 100.0}.data(), 2);
        io.DefineAttribute<int32_t>("pres/scale", 3);

        adios2::Engine w = io.Open(out, adios2::Mode::Write);
        for (size_t s = 0; s < NSTEPS; s++)
        {
            w.BeginStep();
            std::vector<std::vector<double>> tbufs;  // deferred: keep until EndStep
            std::vector<std::vector<float>> ubufs;
            for (int part : parts)
            {
                size_t x0, nx;
                split(NX, nparts, part, x0, nx);
                std::vector<int32_t> p(NY * nx);
                for (size_t y = 0; y < NY; y++)
                    for (size_t i = 0; i < nx; i++)
                        p[y * nx + i] = int32_t(s * 10000 + y * 100 + x0 + i);
                if (nx > 0)
                {
                    pres.SetSelection({{0, x0}, {NY, nx}});
                    w.Put(pres, p.data(), adios2::Mode::Sync);
                }

                for (int b : {part, part + nparts})
                {
                    size_t t0, nt;
                    split(NT, 2 * nparts, b, t0, nt);
                    if (nt == 0)
                        continue;
                    std::vector<double> &tb = tbufs.emplace_back(nt);
                    for (size_t i = 0; i < nt; i++)
                        tb[i] = s * 100.0 + 0.5 * double(t0 + i);
                    temp.SetSelection({{t0}, {nt}});
                    w.Put(temp, tb.data());  // deferred
                }

                size_t u0, nu;
                split(UY, nparts, part, u0, nu);
                if (nu > 0)
                {
                    std::vector<float> &ub = ubufs.emplace_back(nu * UX);
                    for (size_t r = 0; r < nu; r++)
                        for (size_t x = 0; x < UX; x++)
                            ub[r * UX + x] = float(s) + 0.25f * float((u0 + r) * UX + x);
                    u.SetSelection({{u0, 0}, {nu, UX}});
                    w.Put(u, ub.data());
                }

                std::vector<int32_t> l = {part, part};
                w.Put(loc, l.data(), adios2::Mode::Sync);
            }
            if (rank == 0)
            {
                w.Put(p0, 1000.0 + double(s));
                w.Put(label, std::string("step"));
            }
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
        std::cout << "wrote " << out << " (" << nparts << " parts)\n";
#ifdef ZARR_PLUGIN_MPI
    MPI_Finalize();
#endif
    return ret;
}
