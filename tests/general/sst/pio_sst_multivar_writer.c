/*
 * pio_sst_multivar_writer.c
 *
 * ADIOS2 SST streaming test with several variables and frames — C writer.
 * Paired with pio_sst_multivar_reader.
 *
 * Writes NFRAMES records of three record variables over an SST stream:
 *   temp(time, x)     PIO_DOUBLE, 1-D decomposition over x
 *   salt(time, x)     PIO_DOUBLE, shares temp's decomposition
 *   pres(time, y, x)  PIO_INT,    2-D decomposition over (y, x)
 * Each rank owns ELEMENTS_PER_PE consecutive x columns (all y rows).
 */
#include "pio.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include "pio_sst_multivar.h"

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

    const char *stream_name = (argc > 1) ? argv[1] : MV_STREAM_NAME;
    const int nx = nprocs * ELEMENTS_PER_PE;
    int iosysid, ncid, ret;

    ret = PIOc_Init_Intracomm(MPI_COMM_WORLD, nprocs, 1, 0, PIO_REARR_SUBSET, &iosysid);
    ERR(ret);

    /* 1-D decomposition over x */
    int ioid_x;
    PIO_Offset compdof_x[ELEMENTS_PER_PE];
    for (int i = 0; i < ELEMENTS_PER_PE; i++)
        compdof_x[i] = (PIO_Offset)(my_rank * ELEMENTS_PER_PE + i + 1);
    int gdims_x[1] = {nx};
    ret = PIOc_InitDecomp(iosysid, PIO_DOUBLE, 1, gdims_x, ELEMENTS_PER_PE, compdof_x,
                          &ioid_x, NULL, NULL, NULL);
    ERR(ret);

    /* 2-D decomposition over (y, x) */
    int ioid_yx;
    PIO_Offset compdof_yx[NY * ELEMENTS_PER_PE];
    for (int y = 0; y < NY; y++)
        for (int i = 0; i < ELEMENTS_PER_PE; i++)
            compdof_yx[y * ELEMENTS_PER_PE + i] = (PIO_Offset)(y * nx + my_rank * ELEMENTS_PER_PE + i + 1);
    int gdims_yx[2] = {NY, nx};
    ret = PIOc_InitDecomp(iosysid, PIO_INT, 2, gdims_yx, NY * ELEMENTS_PER_PE, compdof_yx,
                          &ioid_yx, NULL, NULL, NULL);
    ERR(ret);

    int iotype = PIO_IOTYPE_ADIOS_SST;
    ret = PIOc_createfile(iosysid, &ncid, &iotype, stream_name, PIO_CLOBBER);
    ERR(ret);

    int dimid_time, dimid_y, dimid_x;
    ret = PIOc_def_dim(ncid, "time", PIO_UNLIMITED, &dimid_time); ERR(ret);
    ret = PIOc_def_dim(ncid, "y", (PIO_Offset)NY, &dimid_y);       ERR(ret);
    ret = PIOc_def_dim(ncid, "x", (PIO_Offset)nx, &dimid_x);       ERR(ret);

    int varid_temp, varid_salt, varid_pres;
    int dims_tx[2]  = {dimid_time, dimid_x};
    int dims_tyx[3] = {dimid_time, dimid_y, dimid_x};
    ret = PIOc_def_var(ncid, "temp", PIO_DOUBLE, 2, dims_tx, &varid_temp);  ERR(ret);
    ret = PIOc_def_var(ncid, "salt", PIO_DOUBLE, 2, dims_tx, &varid_salt);  ERR(ret);
    ret = PIOc_def_var(ncid, "pres", PIO_INT, 3, dims_tyx, &varid_pres);    ERR(ret);
    ret = PIOc_enddef(ncid);
    ERR(ret);

    for (int f = 0; f < NFRAMES; f++)
    {
        double temp[ELEMENTS_PER_PE], salt[ELEMENTS_PER_PE];
        int pres[NY * ELEMENTS_PER_PE];
        for (int i = 0; i < ELEMENTS_PER_PE; i++)
        {
            int x = my_rank * ELEMENTS_PER_PE + i;
            temp[i] = mv_temp(f, x);
            salt[i] = mv_salt(f, x);
        }
        for (int y = 0; y < NY; y++)
            for (int i = 0; i < ELEMENTS_PER_PE; i++)
                pres[y * ELEMENTS_PER_PE + i] = mv_pres(f, y, my_rank * ELEMENTS_PER_PE + i);

        ret = PIOc_setframe(ncid, varid_temp, f); ERR(ret);
        ret = PIOc_write_darray(ncid, varid_temp, ioid_x, ELEMENTS_PER_PE, temp, NULL); ERR(ret);
        ret = PIOc_setframe(ncid, varid_salt, f); ERR(ret);
        ret = PIOc_write_darray(ncid, varid_salt, ioid_x, ELEMENTS_PER_PE, salt, NULL); ERR(ret);
        ret = PIOc_setframe(ncid, varid_pres, f); ERR(ret);
        ret = PIOc_write_darray(ncid, varid_pres, ioid_yx, NY * ELEMENTS_PER_PE, pres, NULL); ERR(ret);
    }

    ret = PIOc_sync(ncid);                  ERR(ret);
    ret = PIOc_closefile(ncid);             ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid_x);  ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid_yx); ERR(ret);
    ret = PIOc_finalize(iosysid);           ERR(ret);

    if (my_rank == 0)
        printf("SST multivar C writer finished (%d frames, 3 variables).\n", NFRAMES);

    MPI_Finalize();
    return 0;
}
