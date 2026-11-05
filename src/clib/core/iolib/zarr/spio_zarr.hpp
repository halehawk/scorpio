#ifndef __SPIO_ZARR_HPP__
#define __SPIO_ZARR_HPP__

/* Native Zarr (v3) iotype, PIO_IOTYPE_ZARR. Write only.
 *
 * Metadata (dims, vars, attributes) is kept in memory on all tasks, like
 * the HDF5 iotype, and written as zarr.json files by io rank 0 at enddef,
 * sync and close. Data is written as uncompressed Zarr chunks with
 * zarr_chunk.hpp: each chunk is assembled on one io task and written by
 * it alone, so io tasks never write the same chunk file.
 *
 * Only io tasks call spio_zarr_write_darray, spio_zarr_put_var,
 * spio_zarr_enddef, spio_zarr_sync and spio_zarr_close; the other
 * functions are called on all tasks. Async mode is not supported.
 */

#include "pio_config.h"
#include "pio.h"
#include "pio_internal.h"

int spio_zarr_create(iosystem_desc_t *ios, file_desc_t *file, const char *filename);
void spio_zarr_free(file_desc_t *file);

int spio_zarr_def_dim(file_desc_t *file, const char *name, PIO_Offset len, int *idp);
int spio_zarr_def_var(file_desc_t *file, const char *name, nc_type xtype, int ndims,
                      const int *dimids, int *varidp);
int spio_zarr_def_var_chunking(file_desc_t *file, int varid, int storage,
                               const PIO_Offset *chunksizesp);
int spio_zarr_put_att(file_desc_t *file, int varid, const char *name, nc_type atttype,
                      PIO_Offset len, nc_type memtype, const void *op);

int spio_zarr_inq(file_desc_t *file, int *ndimsp, int *nvarsp, int *ngattsp, int *unlimdimidp);
int spio_zarr_inq_dim(file_desc_t *file, int dimid, char *name, PIO_Offset *lenp);
int spio_zarr_inq_dimid(file_desc_t *file, const char *name, int *idp);
int spio_zarr_inq_var(file_desc_t *file, int varid, char *name, nc_type *xtypep, int *ndimsp,
                      int *dimidsp, int *nattsp);
int spio_zarr_inq_varid(file_desc_t *file, const char *name, int *varidp);
int spio_zarr_inq_var_chunking(file_desc_t *file, int varid, int *storagep,
                               PIO_Offset *chunksizesp);
int spio_zarr_inq_var_fill(file_desc_t *file, int varid, int *no_fill, void *fill_valuep);
int spio_zarr_inq_att(file_desc_t *file, int varid, const char *name, nc_type *xtypep,
                      PIO_Offset *lenp);
int spio_zarr_inq_attname(file_desc_t *file, int varid, int attnum, char *name);
int spio_zarr_inq_attid(file_desc_t *file, int varid, const char *name, int *idp);

/* Write the blocks of a distributed array held by this io task: block b
 * has file start/count starts[b * fndims ...], counts[b * fndims ...]
 * (record dimension included) and data bufs[b] of type iodesc->piotype.
 * Collective over the io tasks. */
int spio_zarr_write_darray(iosystem_desc_t *ios, file_desc_t *file, int varid,
                           const io_desc_t *iodesc, int fndims, int nblocks,
                           const PIO_Offset *starts, const PIO_Offset *counts,
                           const void *const *bufs);

/* Write start/count of a variable from buf of type memtype (io rank 0
 * writes, the other io tasks do nothing). start/count may be NULL for
 * scalars. */
int spio_zarr_put_var(iosystem_desc_t *ios, file_desc_t *file, int varid,
                      const PIO_Offset *start, const PIO_Offset *count,
                      const PIO_Offset *stride, nc_type memtype, const void *buf);

int spio_zarr_enddef(iosystem_desc_t *ios, file_desc_t *file);
int spio_zarr_sync(iosystem_desc_t *ios, file_desc_t *file);
int spio_zarr_close(iosystem_desc_t *ios, file_desc_t *file);

#endif /* __SPIO_ZARR_HPP__ */
