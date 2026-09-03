/*
 * pio_sst_reader.c
 *
 * ADIOS2 SST streaming test — C reader (connectivity smoke test).
 * Paired with pio_sst_writer; launched together via MPMD mpirun.
 *
 * Opens the SST stream written by pio_sst_writer, verifies the connection
 * succeeds, then closes.  Full per-frame data verification requires the
 * SCORPIO SST read path (see convergence tasks T038/T039).
 *
 * Validates: FR-002, FR-007, SC-002 from the feature spec.
 */
#include "pio.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define ERR(ret) do { \
    if ((ret) != PIO_NOERR) { \
        fprintf(stderr, "PIO error %d at %s:%d\n", (ret), __FILE__, __LINE__); \
        MPI_Abort(MPI_COMM_WORLD, (ret)); \
    } \
} while(0)

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int my_rank, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    const char *stream_name = (argc > 1) ? argv[1] : "scorpio_sst_test_stream";

    int iosysid, ncid, ret;

    ret = PIOc_Init_Intracomm(MPI_COMM_WORLD, nprocs, 1, 0, PIO_REARR_SUBSET, &iosysid);
    ERR(ret);

    /* Open SST stream (reader side) — blocks until writer is available */
    int iotype = PIO_IOTYPE_ADIOS_SST;
    ret = PIOc_openfile(iosysid, &ncid, &iotype, stream_name, PIO_NOWRITE);
    ERR(ret);

    /*
     * SST stream connected successfully.
     * Full per-frame read/verify is deferred to convergence tasks T038/T039
     * (SCORPIO SST read path not yet implemented).
     * Close immediately to release the writer.
     */
    ret = PIOc_closefile(ncid);
    ERR(ret);

    ret = PIOc_finalize(iosysid);
    ERR(ret);

    MPI_Finalize();

    if (my_rank == 0)
        printf("SST C reader finished — stream connection verified.\n");

    return 0;
}
