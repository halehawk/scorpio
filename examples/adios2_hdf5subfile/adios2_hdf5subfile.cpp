#include <iostream>
#include <vector>
#include <mpi.h>
#include <adios2.h>

int main(int argc, char* argv[])
{
    // CRITICAL: Subfiling demands MPI_THREAD_MULTIPLE to manage background I/O threads
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        std::cerr << "Error: MPI implementation does not support MPI_THREAD_MULTIPLE\n";
        MPI_Abort(MPI_COMM_WORLD, -1);
    }

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int ret = 0;

    // Prepare local data
    size_t local_size = 100;
    std::vector<double> my_data(local_size, static_cast<double>(rank));

    try {
        // Initialize ADIOS2 by passing the XML file 
        adios2::ADIOS adios("input.xml", MPI_COMM_WORLD);

        // Fetch the IO group defined in our XML configuration
        adios2::IO h5_io = adios.DeclareIO("HDF5_Subfile_IO");

        // Alternatively, if you didn't use an XML file, you could uncomment the line below:
        // h5_io.SetEngine("HDF5");

        // Define a global 1D array variable 
        adios2::Variable<double> var_data = h5_io.DefineVariable<double>(
            "temperature", 
            {size * local_size},          // Global dimensions
            {rank * local_size},          // Block start offset
            {local_size}                  // Local block dimensions
        );

        // Open the file. Under HDF5 1.14+, this automatically spawns subfiles
        adios2::Engine h5_writer = h5_io.Open("simulation_output.h5", adios2::Mode::Write);

        // Perform a step-based or single block write
        h5_writer.BeginStep();
        h5_writer.Put<double>(var_data, my_data.data());
        h5_writer.EndStep();

        // Properly flush and close out the file handles
        h5_writer.Close();
    }
    catch (std::exception& e) {
        std::cerr << "ADIOS2 Exception on rank " << rank << ": " << e.what() << "\n";
        ret = 1;
    }

    MPI_Finalize();
    return ret;
}

