/*
 * pio_sst_multivar_reader.c
 *
 * ADIOS2 SST streaming test with several variables and frames — C reader.
 * Paired with pio_sst_multivar_writer; see that file for the data layout.
 *
 * Checks each variable's type and number of dimensions, then reads every
 * frame of every variable and verifies all values.
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

static int check_var(int ncid, const char *name, nc_type exp_type, int exp_ndims, int *varidp)
{
    int ret = PIOc_inq_varid(ncid, name, varidp);
    ERR(ret);
    nc_type xtype;
    int ndims;
    ret = PIOc_inq_var(ncid, *varidp, NULL, &xtype, &ndims, NULL, NULL);
    ERR(ret);
    if (xtype != exp_type || ndims != exp_ndims)
    {
        fprintf(stderr, "Variable %s: got type %d ndims %d, expected type %d ndims %d\n",
                name, (int)xtype, ndims, (int)exp_type, exp_ndims);
        return 1;
    }
    return 0;
}

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

    /* Same decompositions as the writer */
    int ioid_x;
    PIO_Offset compdof_x[ELEMENTS_PER_PE];
    for (int i = 0; i < ELEMENTS_PER_PE; i++)
        compdof_x[i] = (PIO_Offset)(my_rank * ELEMENTS_PER_PE + i + 1);
    int gdims_x[1] = {nx};
    ret = PIOc_InitDecomp(iosysid, PIO_DOUBLE, 1, gdims_x, ELEMENTS_PER_PE, compdof_x,
                          &ioid_x, NULL, NULL, NULL);
    ERR(ret);

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
    ret = PIOc_openfile(iosysid, &ncid, &iotype, stream_name, PIO_NOWRITE);
    ERR(ret);

    int errors = 0;
    int varid_temp, varid_salt, varid_pres;
    errors += check_var(ncid, "temp", PIO_DOUBLE, 2, &varid_temp);
    errors += check_var(ncid, "salt", PIO_DOUBLE, 2, &varid_salt);
    errors += check_var(ncid, "pres", PIO_INT, 3, &varid_pres);

    for (int f = 0; f < NFRAMES; f++)
    {
        double temp[ELEMENTS_PER_PE], salt[ELEMENTS_PER_PE];
        int pres[NY * ELEMENTS_PER_PE];

        ret = PIOc_setframe(ncid, varid_temp, f); ERR(ret);
        ret = PIOc_read_darray(ncid, varid_temp, ioid_x, ELEMENTS_PER_PE, temp); ERR(ret);
        ret = PIOc_setframe(ncid, varid_salt, f); ERR(ret);
        ret = PIOc_read_darray(ncid, varid_salt, ioid_x, ELEMENTS_PER_PE, salt); ERR(ret);
        ret = PIOc_setframe(ncid, varid_pres, f); ERR(ret);
        ret = PIOc_read_darray(ncid, varid_pres, ioid_yx, NY * ELEMENTS_PER_PE, pres); ERR(ret);

        for (int i = 0; i < ELEMENTS_PER_PE; i++)
        {
            int x = my_rank * ELEMENTS_PER_PE + i;
            if (temp[i] != mv_temp(f, x))
            {
                fprintf(stderr, "MISMATCH temp: frame=%d rank=%d x=%d: got %g expected %g\n",
                        f, my_rank, x, temp[i], mv_temp(f, x));
                errors++;
            }
            if (salt[i] != mv_salt(f, x))
            {
                fprintf(stderr, "MISMATCH salt: frame=%d rank=%d x=%d: got %g expected %g\n",
                        f, my_rank, x, salt[i], mv_salt(f, x));
                errors++;
            }
            for (int y = 0; y < NY; y++)
            {
                if (pres[y * ELEMENTS_PER_PE + i] != mv_pres(f, y, x))
                {
                    fprintf(stderr, "MISMATCH pres: frame=%d rank=%d y=%d x=%d: got %d expected %d\n",
                            f, my_rank, y, x, pres[y * ELEMENTS_PER_PE + i], mv_pres(f, y, x));
                    errors++;
                }
            }
        }
    }

    if (errors > 0)
    {
        fprintf(stderr, "rank=%d: %d verification errors\n", my_rank, errors);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    ret = PIOc_freedecomp(iosysid, ioid_x);  ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid_yx); ERR(ret);
    ret = PIOc_closefile(ncid);             ERR(ret);
    ret = PIOc_finalize(iosysid);           ERR(ret);

    MPI_Finalize();

    if (my_rank == 0)
        printf("SST multivar C reader finished — %d frames x 3 variables verified OK.\n", NFRAMES);

    return 0;
}
