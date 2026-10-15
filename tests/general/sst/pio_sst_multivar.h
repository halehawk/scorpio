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

#define MV_TITLE "SCORPIO SST multivar test"

/* Writer, define mode: attributes, fill settings, and rename/delete calls.
 * Renames use the same name so the expected structure stays the same for
 * every iotype (ADIOS iotypes do not support renaming). */
static int mv_define_extras(int ncid, int dimid_x, int varid_temp, int varid_pres, const char *who)
{
    int errors = 0, ret;
    const int valid_range[2] = {0, 1000000};

    /* "title" first, so it is attribute 0 of PIO_GLOBAL for every iotype */
    ret = PIOc_put_att_text(ncid, PIO_GLOBAL, "title", strlen(MV_TITLE), MV_TITLE);
    MV_CHECK(ret == PIO_NOERR, "put_att title failed (%d)", ret);
    ret = PIOc_put_att_text(ncid, varid_temp, "units", 1, "K");
    MV_CHECK(ret == PIO_NOERR, "put_att temp:units failed (%d)", ret);
    ret = PIOc_put_att_int(ncid, varid_pres, "valid_range", PIO_INT, 2, valid_range);
    MV_CHECK(ret == PIO_NOERR, "put_att pres:valid_range failed (%d)", ret);

    /* PIOc_createfile sets NOFILL, so that is the old mode */
    int old_mode = -1;
    ret = PIOc_set_fill(ncid, PIO_NOFILL, &old_mode);
    MV_CHECK(ret == PIO_NOERR && old_mode == PIO_NOFILL, "set_fill: old mode %d (ret %d), expected %d",
             old_mode, ret, PIO_NOFILL);
    ret = PIOc_def_var_fill(ncid, varid_temp, PIO_NOFILL, NULL);
    MV_CHECK(ret == PIO_NOERR, "def_var_fill failed (%d)", ret);

    ret = PIOc_rename_dim(ncid, dimid_x, "x");
    MV_CHECK(ret == PIO_NOERR, "rename_dim failed (%d)", ret);
    ret = PIOc_rename_var(ncid, varid_temp, "temp");
    MV_CHECK(ret == PIO_NOERR, "rename_var failed (%d)", ret);
    ret = PIOc_rename_att(ncid, varid_temp, "units", "units");
    MV_CHECK(ret == PIO_NOERR, "rename_att failed (%d)", ret);
    ret = PIOc_put_att_text(ncid, PIO_GLOBAL, "scratch", 1, "s");
    MV_CHECK(ret == PIO_NOERR, "put_att scratch failed (%d)", ret);
    ret = PIOc_del_att(ncid, PIO_GLOBAL, "scratch");
    MV_CHECK(ret == PIO_NOERR, "del_att failed (%d)", ret);

    return errors;
}

static int mv_check_attrs(int ncid, int varid_temp, const char *who);

/* Format, unlimited dimensions, variable fill and, when check_attrs is set,
 * the attributes from mv_define_extras. Returns the number of mismatches. */
static int mv_check_extras(int ncid, int check_attrs, const char *who)
{
    int errors = 0, ret;

    int format = -1;
    ret = PIOc_inq_format(ncid, &format);
    MV_CHECK(ret == PIO_NOERR && format == 1, "inq_format = %d (ret %d), expected 1 (as for the ADIOS iotype)",
             format, ret);

    int varid_temp = -1, no_fill = -1;
    ret = PIOc_inq_varid(ncid, "temp", &varid_temp);
    MV_CHECK(ret == PIO_NOERR, "inq_varid(temp) failed (%d)", ret);
    ret = PIOc_inq_var_fill(ncid, varid_temp, &no_fill, NULL);
    MV_CHECK(ret == PIO_NOERR, "inq_var_fill failed (%d)", ret);

    if (check_attrs)
        errors += mv_check_attrs(ncid, varid_temp, who);

    /* Last, so a regression here cannot hide the checks above */
    int nunlimdims = -1, unlimdimids[PIO_MAX_DIMS], dimid_time = -2;
    for (int d = 0; d < PIO_MAX_DIMS; d++)
        unlimdimids[d] = -1;
    PIOc_inq_dimid(ncid, "time", &dimid_time);
    ret = PIOc_inq_unlimdims(ncid, &nunlimdims, unlimdimids);
    MV_CHECK(ret == PIO_NOERR && nunlimdims == 1 && unlimdimids[0] == dimid_time,
             "inq_unlimdims: %d dims, first %d (ret %d), expected 1 dim, %d (time)",
             nunlimdims, unlimdimids[0], ret, dimid_time);

    return errors;
}

/* The attributes defined by mv_define_extras. Returns the number of mismatches. */
static int mv_check_attrs(int ncid, int varid_temp, const char *who)
{
    int errors = 0, ret;
    int varid_pres = -1;
    ret = PIOc_inq_varid(ncid, "pres", &varid_pres);
    MV_CHECK(ret == PIO_NOERR, "inq_varid(pres) failed (%d)", ret);

    struct { int varid; const char *name; nc_type type; PIO_Offset len; } atts[3] = {
        {PIO_GLOBAL, "title",       PIO_CHAR, (PIO_Offset)strlen(MV_TITLE)},
        {varid_temp, "units",       PIO_CHAR, 1},
        {varid_pres, "valid_range", PIO_INT,  2},
    };
    for (int a = 0; a < 3; a++)
    {
        nc_type xtype = PIO_NAT;
        PIO_Offset len = -1;
        ret = PIOc_inq_att(ncid, atts[a].varid, atts[a].name, &xtype, &len);
        MV_CHECK(ret == PIO_NOERR && xtype == atts[a].type && len == atts[a].len,
                 "inq_att(%s): type %d len %lld (ret %d), expected type %d len %lld", atts[a].name,
                 (int)xtype, (long long)len, ret, (int)atts[a].type, (long long)atts[a].len);
    }

    int attid = -1;
    ret = PIOc_inq_attid(ncid, PIO_GLOBAL, "title", &attid);
    MV_CHECK(ret == PIO_NOERR && attid == 0, "inq_attid(title) = %d (ret %d), expected 0", attid, ret);

    char attname[PIO_MAX_NAME + 1] = {0};
    ret = PIOc_inq_attname(ncid, PIO_GLOBAL, 0, attname);
    MV_CHECK(ret == PIO_NOERR && strcmp(attname, "title") == 0,
             "inq_attname(global, 0) = \"%s\" (ret %d), expected \"title\"", attname, ret);

    return errors;
}

#endif
