// Reads simulation_output.h5 back with the ADIOS2 HDF5 engine and checks
// that each rank's block of "temperature" holds that rank's number.
#include <iostream>
#include <vector>
#include <mpi.h>
#include <adios2.h>

int main(int argc, char* argv[])
{
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    size_t local_size = 100;
    int nerr = 0;

    try {
        adios2::ADIOS adios("input.xml", MPI_COMM_WORLD);
        adios2::IO h5_io = adios.DeclareIO("HDF5_Subfile_IO");
        adios2::Engine h5_reader = h5_io.Open("simulation_output.h5", adios2::Mode::Read);

        h5_reader.BeginStep();
        adios2::Variable<double> var_data = h5_io.InquireVariable<double>("temperature");
        if (!var_data || var_data.Shape()[0] != size * local_size) {
            std::cerr << "rank " << rank << ": temperature missing or wrong shape\n";
            nerr++;
        } else {
            var_data.SetSelection({{rank * local_size}, {local_size}});
            std::vector<double> data(local_size, -1.0);
            h5_reader.Get<double>(var_data, data.data(), adios2::Mode::Sync);
            for (size_t i = 0; i < local_size; i++)
                if (data[i] != static_cast<double>(rank))
                    nerr++;
        }
        h5_reader.EndStep();
        h5_reader.Close();
    }
    catch (std::exception& e) {
        std::cerr << "ADIOS2 Exception on rank " << rank << ": " << e.what() << "\n";
        nerr++;
    }

    int total = 0;
    MPI_Allreduce(&nerr, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0)
        std::cout << (total ? "FAIL" : "PASS") << ": " << total << " bad values\n";

    MPI_Finalize();
    return total ? 1 : 0;
}
