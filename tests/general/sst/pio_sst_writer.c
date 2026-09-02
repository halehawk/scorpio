/*
 * pio_sst_writer.c
 *
 * ADIOS2 SST streaming test — C writer.
 * Paired with pio_sst_reader; launched together via MPMD mpirun.
 *
 * All ranks in this job act as the SST writer.  Opens an SST stream,
 * defines a 1-D distributed integer variable, writes NFRAMES timesteps,
 * then closes the stream.
 *
 * Validates: FR-002, FR-003, FR-007, SC-002, SC-005 from the feature spec.
 */
#include "config.h"
#include "pio.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define NDIMS           1
#define NFRAMES         3
#define ELEMENTS_PER_PE 4

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

    int iosysid, ncid, ioid, varid, dimid, ret;

    /* All ranks participate as I/O tasks */
    ret = PIOc_Init_Intracomm(MPI_COMM_WORLD, nprocs, 1, 0, PIO_REARR_SUBSET, &iosysid);
    ERR(ret);

    /* 1-D decomposition: each rank owns ELEMENTS_PER_PE consecutive elements (1-based) */
    int gdim = nprocs * ELEMENTS_PER_PE;
    PIO_Offset *compdof = (PIO_Offset *)malloc(ELEMENTS_PER_PE * sizeof(PIO_Offset));
    for (int i = 0; i < ELEMENTS_PER_PE; i++)
        compdof[i] = (PIO_Offset)(my_rank * ELEMENTS_PER_PE + i + 1);
    ret = PIOc_InitDecomp(iosysid, PIO_INT, NDIMS, &gdim,
                          ELEMENTS_PER_PE, compdof, &ioid, NULL, NULL, NULL);
    ERR(ret);
    free(compdof);

    /* Open SST stream (writer side) */
    int iotype = PIO_IOTYPE_ADIOS_SST;
    ret = PIOc_createfile(iosysid, &ncid, &iotype, stream_name, PIO_CLOBBER);
    ERR(ret);

    /* Define global dimension and variable */
    ret = PIOc_def_dim(iosysid, ncid, "x", (PIO_Offset)gdim, &dimid);
    ERR(ret);
    ret = PIOc_def_var(iosysid, ncid, "data", PIO_INT, NDIMS, &dimid, &varid);
    ERR(ret);
    ret = PIOc_enddef(iosysid, ncid);
    ERR(ret);

    /* Write NFRAMES timesteps */
    for (int t = 0; t < NFRAMES; t++) {
        int data[ELEMENTS_PER_PE];
        for (int i = 0; i < ELEMENTS_PER_PE; i++)
            data[i] = t * 1000 + my_rank * ELEMENTS_PER_PE + i;

        ret = PIOc_setframe(iosysid, ncid, varid, t);
        ERR(ret);
        ret = PIOc_write_darray(iosysid, ncid, varid, ioid,
                                ELEMENTS_PER_PE, data, NULL);
        ERR(ret);
    }

    ret = PIOc_sync(iosysid, ncid);
    ERR(ret);
    ret = PIOc_closefile(iosysid, ncid);
    ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid);
    ERR(ret);
    ret = PIOc_finalize(iosysid);
    ERR(ret);

    if (my_rank == 0)
        printf("SST C writer finished.\n");

    MPI_Finalize();
    return 0;
}
