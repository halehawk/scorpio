/*
 * pio_bp_pyreader_writer.c
 *
 * Writes an ADIOS BP file (PIO_IOTYPE_ADIOS) that exercises the read paths of
 * tools/adios2py/spio_adios2_reader.py; checked by
 * check_spio_adios2_reader_paths.py. Run with 4 ranks: 2 I/O tasks make each
 * ADIOS block merge 2 ranks, and NFRAMES > PIO_MAX_CACHED_STEPS_FOR_ADIOS
 * (128 by default) spreads the frames over several ADIOS steps.
 *
 *   temp(time, x)    double darray, every frame
 *   sparse(time, x)  int darray, only even x written, fill value FILL_INT
 *   mask(x)          int darray, written once (no record dimension)
 *   xcoord(x)        double put_var
 *   p0               double put_var scalar
 *   step(time)       int put_vara, one record per frame
 *   label(nchars)    char put_var_text
 */
#include "pio.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFRAMES         300
#define ELEMENTS_PER_PE 4
#define NCHARS          8
#define FILL_INT        -999
#define LABEL           "SCORPIO!"

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

    const char *fname = (argc > 1) ? argv[1] : "spio_bp_pyreader";
    const int nx = nprocs * ELEMENTS_PER_PE;
    const int niotasks = (nprocs > 1) ? nprocs / 2 : 1;
    int iosysid, ncid, ret;

    ret = PIOc_Init_Intracomm(MPI_COMM_WORLD, niotasks, 1, 0, PIO_REARR_SUBSET, &iosysid);
    ERR(ret);

    /* Full 1-D decompositions over x, for double and int */
    PIO_Offset compdof[ELEMENTS_PER_PE];
    for (int i = 0; i < ELEMENTS_PER_PE; i++)
        compdof[i] = (PIO_Offset)(my_rank * ELEMENTS_PER_PE + i + 1);
    int gdims[1] = {nx};
    int ioid_d, ioid_i;
    ret = PIOc_InitDecomp(iosysid, PIO_DOUBLE, 1, gdims, ELEMENTS_PER_PE, compdof, &ioid_d, NULL, NULL, NULL);
    ERR(ret);
    ret = PIOc_InitDecomp(iosysid, PIO_INT, 1, gdims, ELEMENTS_PER_PE, compdof, &ioid_i, NULL, NULL, NULL);
    ERR(ret);

    /* Sparse decomposition: only the even global x of each rank (ELEMENTS_PER_PE is even) */
    PIO_Offset sparse_dof[ELEMENTS_PER_PE / 2];
    for (int i = 0; i < ELEMENTS_PER_PE / 2; i++)
        sparse_dof[i] = (PIO_Offset)(my_rank * ELEMENTS_PER_PE + 2 * i + 1);
    int ioid_sparse;
    ret = PIOc_InitDecomp(iosysid, PIO_INT, 1, gdims, ELEMENTS_PER_PE / 2, sparse_dof, &ioid_sparse,
                          NULL, NULL, NULL);
    ERR(ret);

    int iotype = PIO_IOTYPE_ADIOS;
    ret = PIOc_createfile(iosysid, &ncid, &iotype, fname, PIO_CLOBBER);
    ERR(ret);

    int dimid_time, dimid_x, dimid_c;
    ret = PIOc_def_dim(ncid, "time", PIO_UNLIMITED, &dimid_time); ERR(ret);
    ret = PIOc_def_dim(ncid, "x", (PIO_Offset)nx, &dimid_x);       ERR(ret);
    ret = PIOc_def_dim(ncid, "nchars", NCHARS, &dimid_c);         ERR(ret);

    int dims_tx[2] = {dimid_time, dimid_x};
    int v_temp, v_sparse, v_mask, v_xcoord, v_p0, v_step, v_label;
    ret = PIOc_def_var(ncid, "temp", PIO_DOUBLE, 2, dims_tx, &v_temp);       ERR(ret);
    ret = PIOc_def_var(ncid, "sparse", PIO_INT, 2, dims_tx, &v_sparse);      ERR(ret);
    ret = PIOc_def_var(ncid, "mask", PIO_INT, 1, &dimid_x, &v_mask);         ERR(ret);
    ret = PIOc_def_var(ncid, "xcoord", PIO_DOUBLE, 1, &dimid_x, &v_xcoord);  ERR(ret);
    ret = PIOc_def_var(ncid, "p0", PIO_DOUBLE, 0, NULL, &v_p0);              ERR(ret);
    ret = PIOc_def_var(ncid, "step", PIO_INT, 1, &dimid_time, &v_step);      ERR(ret);
    ret = PIOc_def_var(ncid, "label", PIO_CHAR, 1, &dimid_c, &v_label);      ERR(ret);
    ret = PIOc_enddef(ncid);
    ERR(ret);

    /* put_var: the same data on every rank (collective calls) */
    double xcoord[nx];
    for (int x = 0; x < nx; x++)
        xcoord[x] = 0.25 * x;
    ret = PIOc_put_var_double(ncid, v_xcoord, xcoord); ERR(ret);
    double p0 = 1000.5;
    ret = PIOc_put_var_double(ncid, v_p0, &p0);        ERR(ret);
    ret = PIOc_put_var_text(ncid, v_label, LABEL);     ERR(ret);

    /* Non-record darray, written once */
    int mask[ELEMENTS_PER_PE];
    for (int i = 0; i < ELEMENTS_PER_PE; i++)
        mask[i] = (my_rank * ELEMENTS_PER_PE + i) % 3;
    ret = PIOc_write_darray(ncid, v_mask, ioid_i, ELEMENTS_PER_PE, mask, NULL); ERR(ret);

    int fill = FILL_INT;
    for (int f = 0; f < NFRAMES; f++)
    {
        double temp[ELEMENTS_PER_PE];
        int sparse[ELEMENTS_PER_PE / 2];
        for (int i = 0; i < ELEMENTS_PER_PE; i++)
            temp[i] = f * 1000.0 + my_rank * ELEMENTS_PER_PE + i + 0.5;
        for (int i = 0; i < ELEMENTS_PER_PE / 2; i++)
            sparse[i] = f * 100 + my_rank * ELEMENTS_PER_PE + 2 * i;

        ret = PIOc_setframe(ncid, v_temp, f); ERR(ret);
        ret = PIOc_write_darray(ncid, v_temp, ioid_d, ELEMENTS_PER_PE, temp, NULL); ERR(ret);
        ret = PIOc_setframe(ncid, v_sparse, f); ERR(ret);
        ret = PIOc_write_darray(ncid, v_sparse, ioid_sparse, ELEMENTS_PER_PE / 2, sparse, &fill); ERR(ret);

        PIO_Offset start = f, count = 1;
        int step = 7 * f + 1;
        ret = PIOc_put_vara_int(ncid, v_step, &start, &count, &step); ERR(ret);
    }

    ret = PIOc_closefile(ncid);                  ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid_d);      ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid_i);      ERR(ret);
    ret = PIOc_freedecomp(iosysid, ioid_sparse); ERR(ret);
    ret = PIOc_finalize(iosysid);                ERR(ret);

    if (my_rank == 0)
        printf("BP pyreader writer finished (%d ranks, %d I/O tasks, %d frames).\n", nprocs, niotasks, NFRAMES);

    MPI_Finalize();
    return 0;
}
