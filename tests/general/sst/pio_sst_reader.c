/*
 * pio_sst_reader.c
 *
 * ADIOS2 SST streaming test — C reader (data read and verification).
 * Paired with pio_sst_writer; launched via separate mpirun (background writer).
 *
 * Opens the SST stream written by pio_sst_writer, reads NFRAMES timesteps of
 * a 1-D integer variable, verifies the values match the expected pattern, then
 * closes the stream.
 *
 * Validates: FR-002, FR-003, FR-007, FR-008, SC-002, SC-005 from the feature spec.
 */
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

    int iosysid, ncid, ioid, varid, ret;

    ret = PIOc_Init_Intracomm(MPI_COMM_WORLD, nprocs, 1, 0, PIO_REARR_SUBSET, &iosysid);
    ERR(ret);

    /* 1-D decomposition matching the writer: each rank owns ELEMENTS_PER_PE consecutive
     * elements with 1-based global indices */
    int gdim = nprocs * ELEMENTS_PER_PE;
    PIO_Offset *compdof = (PIO_Offset *)malloc(ELEMENTS_PER_PE * sizeof(PIO_Offset));
    for (int i = 0; i < ELEMENTS_PER_PE; i++)
        compdof[i] = (PIO_Offset)(my_rank * ELEMENTS_PER_PE + i + 1);
    ret = PIOc_InitDecomp(iosysid, PIO_INT, NDIMS, &gdim,
                          ELEMENTS_PER_PE, compdof, &ioid, NULL, NULL, NULL);
    ERR(ret);
    free(compdof);

    /* Open SST stream (reader side) — blocks until writer is available */
    int iotype = PIO_IOTYPE_ADIOS_SST;
    ret = PIOc_openfile(iosysid, &ncid, &iotype, stream_name, PIO_NOWRITE);
    ERR(ret);

    /* Locate the variable written by the writer */
    ret = PIOc_inq_varid(ncid, "data", &varid);
    ERR(ret);

    /* Read and verify NFRAMES timesteps */
    int errors = 0;
    for (int t = 0; t < NFRAMES; t++) {
        int buf[ELEMENTS_PER_PE];

        ret = PIOc_setframe(ncid, varid, t);
        ERR(ret);

        ret = PIOc_read_darray(ncid, varid, ioid, ELEMENTS_PER_PE, buf, NULL);
        ERR(ret);

        for (int i = 0; i < ELEMENTS_PER_PE; i++) {
            int expected = t * 1000 + my_rank * ELEMENTS_PER_PE + i;
            if (buf[i] != expected) {
                fprintf(stderr,
                        "MISMATCH: frame=%d rank=%d i=%d: got %d expected %d\n",
                        t, my_rank, i, buf[i], expected);
                errors++;
            }
        }
    }

    if (errors > 0) {
        fprintf(stderr, "rank=%d: %d verification errors\n", my_rank, errors);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    ret = PIOc_freedecomp(iosysid, ioid);
    ERR(ret);

    ret = PIOc_closefile(ncid);
    ERR(ret);

    ret = PIOc_finalize(iosysid);
    ERR(ret);

    MPI_Finalize();

    if (my_rank == 0)
        printf("SST C reader finished — all %d frames verified OK.\n", NFRAMES);

    return 0;
}
