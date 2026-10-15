/* Shared sizes, expected values and structure check for
 * pio_sst_multivar_writer/reader. */
#ifndef PIO_SST_MULTIVAR_H
#define PIO_SST_MULTIVAR_H

#include "pio.h"
#include <stdio.h>
#include <string.h>

#define NFRAMES         10
#define NY              3
#define ELEMENTS_PER_PE 4
#define MV_STREAM_NAME  "scorpio_sst_multivar_stream"

/* x is the global (0-based) x index; all values are exact in double/int */
static inline double mv_temp(int f, int x)        { return f * 1000.0 + x + 0.5; }
static inline double mv_salt(int f, int x)        { return -(f * 1000.0 + x) - 0.25; }
static inline int    mv_pres(int f, int y, int x) { return f * 10000 + y * 100 + x; }

#define MV_CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "STRUCTURE (%s): ", who); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
    errors++; } } while (0)

/* Check dimensions and variables through the PIO inquiry API, the way a model
 * would while writing or reading. The unlimited dimension's length is not
 * checked: ADIOS iotypes report it as PIO_UNLIMITED, not the record count.
 * Returns the number of mismatches. */
static int mv_check_structure(int ncid, int nx, const char *who)
{
    int errors = 0, ret;

    int ndims, nvars, ngatts, unlimdimid;
    ret = PIOc_inq(ncid, &ndims, &nvars, &ngatts, &unlimdimid);
    MV_CHECK(ret == PIO_NOERR, "PIOc_inq failed (%d)", ret);
    MV_CHECK(ndims == 3, "ndims = %d, expected 3", ndims);
    MV_CHECK(nvars == 3, "nvars = %d, expected 3", nvars);

    const char *dim_names[3] = {"time", "y", "x"};
    const PIO_Offset dim_lens[3] = {0 /* unchecked */, NY, nx};
    int dimids[3];
    for (int d = 0; d < 3; d++)
    {
        ret = PIOc_inq_dimid(ncid, dim_names[d], &dimids[d]);
        MV_CHECK(ret == PIO_NOERR, "PIOc_inq_dimid(%s) failed (%d)", dim_names[d], ret);

        char name[PIO_MAX_NAME + 1] = {0};
        ret = PIOc_inq_dimname(ncid, dimids[d], name);
        MV_CHECK(ret == PIO_NOERR && strcmp(name, dim_names[d]) == 0,
                 "PIOc_inq_dimname(%d) = \"%s\" (ret %d), expected \"%s\"", dimids[d], name, ret, dim_names[d]);

        if (d > 0)
        {
            PIO_Offset len = -1;
            ret = PIOc_inq_dimlen(ncid, dimids[d], &len);
            MV_CHECK(ret == PIO_NOERR && len == dim_lens[d],
                     "PIOc_inq_dimlen(%s) = %lld (ret %d), expected %lld",
                     dim_names[d], (long long)len, ret, (long long)dim_lens[d]);
        }
    }
    MV_CHECK(unlimdimid == dimids[0], "unlimdimid = %d, expected %d (time)", unlimdimid, dimids[0]);

    /* Each variable: type, number of dims, and dimension names via its dimids */
    struct { const char *name; nc_type type; int ndims; int dims[3]; } vars[3] = {
        {"temp", PIO_DOUBLE, 2, {0, 2}},     /* indexes into dim_names */
        {"salt", PIO_DOUBLE, 2, {0, 2}},
        {"pres", PIO_INT,    3, {0, 1, 2}},
    };
    for (int v = 0; v < 3; v++)
    {
        int varid, vndims = -1, vdimids[3] = {-1, -1, -1};
        nc_type xtype = PIO_NAT;
        ret = PIOc_inq_varid(ncid, vars[v].name, &varid);
        MV_CHECK(ret == PIO_NOERR, "PIOc_inq_varid(%s) failed (%d)", vars[v].name, ret);
        if (ret != PIO_NOERR)
            continue;

        ret = PIOc_inq_vartype(ncid, varid, &xtype);
        MV_CHECK(ret == PIO_NOERR && xtype == vars[v].type, "%s: type %d (ret %d), expected %d",
                 vars[v].name, (int)xtype, ret, (int)vars[v].type);
        ret = PIOc_inq_varndims(ncid, varid, &vndims);
        MV_CHECK(ret == PIO_NOERR && vndims == vars[v].ndims, "%s: ndims %d (ret %d), expected %d",
                 vars[v].name, vndims, ret, vars[v].ndims);
        if (ret != PIO_NOERR || vndims != vars[v].ndims)
            continue;

        ret = PIOc_inq_vardimid(ncid, varid, vdimids);
        MV_CHECK(ret == PIO_NOERR, "%s: PIOc_inq_vardimid failed (%d)", vars[v].name, ret);
        for (int d = 0; d < vndims; d++)
        {
            const char *exp_name = dim_names[vars[v].dims[d]];
            char name[PIO_MAX_NAME + 1] = {0};
            ret = PIOc_inq_dimname(ncid, vdimids[d], name);
            MV_CHECK(ret == PIO_NOERR && strcmp(name, exp_name) == 0,
                     "%s: dimension %d is \"%s\" (dimid %d, ret %d), expected \"%s\"",
                     vars[v].name, d, name, vdimids[d], ret, exp_name);
        }
    }
    return errors;
}

#endif
