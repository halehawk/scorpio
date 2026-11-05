/**
 * @file
 * Native Zarr (v3) iotype, PIO_IOTYPE_ZARR. See spio_zarr.hpp.
 *
 * Store layout (Zarr v3, uncompressed, little endian):
 *   <file>/zarr.json              group, global attributes
 *   <file>/<var>/zarr.json        array: shape, chunks, fill value,
 *                                 dimension_names, attributes
 *   <file>/<var>/c/i/j/...        chunks
 * The unlimited dimension has chunk size 1, so each record is written to
 * its own chunks. Other chunk sizes are the ones set with
 * PIOc_def_var_chunking, else the ones zarr-python would choose
 * (spio_guess_zarr_chunks; for distributed arrays from the decomposition,
 * which with PIOc_set_zarr_decomp gives each io task whole chunks).
 */
#include "pio_config.h"

/* Empty unless the Zarr iotype is enabled (PIO_ENABLE_ZARR) */
#ifdef _SPIO_ZARR

#include "spio_zarr.hpp"
#include "zarr_chunk_mpi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct ZAtt {
    std::string name;
    nc_type type;
    PIO_Offset len;
    std::string json; /* value as JSON */
};

struct ZDim {
    std::string name;
    PIO_Offset len; /* PIO_UNLIMITED for the unlimited dimension */
};

struct ZVar {
    std::string name;
    nc_type type;
    std::vector<int> dimids;
    std::vector<ZAtt> atts;
    std::vector<size_t> chunks; /* empty until set or first written */
    std::string fill_json;      /* _FillValue as JSON, empty if none */
};

struct ZFile {
    std::string path;
    std::vector<ZDim> dims;
    std::vector<ZVar> vars;
    std::vector<ZAtt> gatts;
    size_t nrecs = 0; /* records written (local to this task until close) */
};

ZFile *zfile(file_desc_t *file)
{
    return static_cast<ZFile *>(file->zarr_file);
}

ZVar *zvar(file_desc_t *file, int varid)
{
    ZFile *zf = zfile(file);
    if (!zf || varid < 0 || varid >= (int)zf->vars.size())
        return nullptr;
    return &zf->vars[varid];
}

/* Call f(T()) with T the C type stored for a NetCDF variable or
 * attribute type (NC_CHAR is stored as uint8). */
template <typename F>
int dispatch_type(nc_type t, F &&f)
{
    switch (t) {
    case NC_BYTE: f(int8_t()); break;
    case NC_CHAR:
    case NC_UBYTE: f(uint8_t()); break;
    case NC_SHORT: f(int16_t()); break;
    case NC_USHORT: f(uint16_t()); break;
    case NC_INT: f(int32_t()); break;
    case NC_UINT: f(uint32_t()); break;
    case NC_INT64: f(int64_t()); break;
    case NC_UINT64: f(uint64_t()); break;
    case NC_FLOAT: f(float()); break;
    case NC_DOUBLE: f(double()); break;
    default: return PIO_EBADTYPE;
    }
    return PIO_NOERR;
}

/* Same for the type of user data in memory, which may also be long. */
template <typename F>
int dispatch_mem_type(nc_type t, F &&f)
{
    if (t == PIO_LONG_INTERNAL) {
        f(long());
        return PIO_NOERR;
    }
    return dispatch_type(t, std::forward<F>(f));
}

/* Convert n values of NetCDF type memtype at buf to T. */
template <typename T>
int convert(nc_type memtype, const void *buf, size_t n, std::vector<T> &out)
{
    out.resize(n);
    return dispatch_mem_type(memtype, [&](auto m) {
        using M = decltype(m);
        const M *src = static_cast<const M *>(buf);
        for (size_t i = 0; i < n; i++)
            out[i] = static_cast<T>(src[i]);
    });
}

std::string json_string(const char *s, size_t n)
{
    std::string out = "\"";
    for (size_t i = 0; i < n && s[i] != '\0'; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else
                out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string json_string(const std::string &s)
{
    return json_string(s.c_str(), s.size());
}

/* JSON for an attribute value: a string for NC_CHAR, else a number (len 1)
 * or a list of numbers, converted from memtype to atttype. */
int att_json(nc_type atttype, PIO_Offset len, nc_type memtype, const void *op, std::string &json)
{
    if (atttype == NC_CHAR) {
        json = json_string(static_cast<const char *>(op), (size_t)len);
        return PIO_NOERR;
    }
    if (memtype == NC_CHAR)
        return PIO_EBADTYPE;
    return dispatch_type(atttype, [&](auto a) {
        using A = decltype(a);
        std::vector<A> v;
        if (convert(memtype, op, (size_t)len, v) != PIO_NOERR)
            return;
        json = (len == 1) ? "" : "[";
        for (size_t i = 0; i < v.size(); i++)
            json += (i ? ", " : "") + zarr_chunk::detail::json_number(v[i]);
        if (len != 1)
            json += "]";
    });
}

/* A float _FillValue attribute as xarray writes it in Zarr v3 stores:
 * base64 of the value as a little-endian double. */
std::string fill_attr_json(double value)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char in[9] = {0};
    memcpy(in, &value, 8); /* little-endian host assumed, as for the chunks */
    std::string out = "\"";
    for (int i = 0; i < 9; i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += (i + 1 < 8) ? b64[(v >> 6) & 63] : '=';
        out += (i + 2 < 8) ? b64[v & 63] : '=';
    }
    return out + "\"";
}

/* NetCDF's default fill value for a type, as JSON. */
std::string default_fill_json(nc_type t)
{
    switch (t) {
    case NC_BYTE: return zarr_chunk::detail::json_number((int8_t)PIO_FILL_BYTE);
    case NC_CHAR: return "0";
    case NC_UBYTE: return zarr_chunk::detail::json_number((uint8_t)PIO_FILL_UBYTE);
    case NC_SHORT: return zarr_chunk::detail::json_number((int16_t)PIO_FILL_SHORT);
    case NC_USHORT: return zarr_chunk::detail::json_number((uint16_t)PIO_FILL_USHORT);
    case NC_INT: return zarr_chunk::detail::json_number((int32_t)PIO_FILL_INT);
    case NC_UINT: return zarr_chunk::detail::json_number((uint32_t)PIO_FILL_UINT);
    case NC_INT64: return zarr_chunk::detail::json_number((int64_t)PIO_FILL_INT64);
    case NC_UINT64: return zarr_chunk::detail::json_number((uint64_t)PIO_FILL_UINT64);
    case NC_FLOAT: return zarr_chunk::detail::json_number((float)PIO_FILL_FLOAT);
    default: return zarr_chunk::detail::json_number((double)PIO_FILL_DOUBLE);
    }
}

/* The fill value of a variable in its type. */
template <typename T>
T fill_value(const ZVar &v)
{
    const std::string &s = v.fill_json.empty() ? default_fill_json(v.type) : v.fill_json;
    if (s == "\"NaN\"")
        return static_cast<T>(NAN);
    if (s == "\"Infinity\"")
        return static_cast<T>(INFINITY);
    if (s == "\"-Infinity\"")
        return static_cast<T>(-INFINITY);
    if (std::is_floating_point<T>::value)
        return static_cast<T>(std::stod(s));
    if (std::is_signed<T>::value)
        return static_cast<T>(std::stoll(s));
    return static_cast<T>(std::stoull(s));
}

bool is_unlimited(const ZFile &zf, int dimid)
{
    return zf.dims[dimid].len == PIO_UNLIMITED;
}

/* Array shape, with the unlimited dimension at nrecs. */
std::vector<size_t> var_shape(const ZFile &zf, const ZVar &v, size_t nrecs)
{
    std::vector<size_t> shape;
    for (int d : v.dimids)
        shape.push_back(is_unlimited(zf, d) ? nrecs : (size_t)zf.dims[d].len);
    return shape;
}

/* Default chunks of a variable: 1 along the unlimited dimension, the
 * zarr-python guess for the other dimensions. typesize and the other
 * dimension sizes may come from a decomposition (dims, ndims). */
std::vector<size_t> guess_chunks(const ZFile &zf, const ZVar &v, int typesize,
                                 const int *dims = nullptr, int ndims = 0)
{
    std::vector<PIO_Offset> fixed, guess;
    if (dims) {
        fixed.assign(dims, dims + ndims);
    } else {
        for (int d : v.dimids)
            if (!is_unlimited(zf, d))
                fixed.push_back(zf.dims[d].len);
    }
    guess.resize(fixed.size());
    spio_guess_zarr_chunks((int)fixed.size(), fixed.data(), typesize, guess.data());

    /* Decomposition dims may have extra outermost dims of length 1; keep
     * the innermost ones. */
    size_t nfixed = 0;
    for (int d : v.dimids)
        nfixed += !is_unlimited(zf, d);
    std::vector<size_t> chunks;
    size_t g = guess.size() - std::min(guess.size(), nfixed);
    for (int d : v.dimids)
        chunks.push_back(is_unlimited(zf, d) ? 1 : (size_t)std::max<PIO_Offset>(1, guess[g++]));
    return chunks;
}

int type_size(nc_type t)
{
    int sz = 0;
    dispatch_type(t, [&](auto x) { sz = (int)sizeof(x); });
    return sz;
}

std::string atts_json(const std::vector<ZAtt> &atts)
{
    std::string s = "{";
    for (size_t i = 0; i < atts.size(); i++)
        s += (i ? ", " : "") + json_string(atts[i].name) + ": " + atts[i].json;
    return s + "}";
}

/* Write all zarr.json files (io rank 0). */
int write_metadata(ZFile &zf, size_t nrecs)
{
    try {
        fs::create_directories(zf.path);
        std::ofstream(fs::path(zf.path) / "zarr.json")
            << "{\n  \"zarr_format\": 3,\n  \"node_type\": \"group\",\n  \"attributes\": "
            << atts_json(zf.gatts) << "\n}\n";

        for (ZVar &v : zf.vars) {
            std::vector<size_t> chunks = v.chunks;
            if (chunks.empty())
                chunks = guess_chunks(zf, v, type_size(v.type));
            std::vector<std::string> dim_names;
            for (int d : v.dimids)
                dim_names.push_back(zf.dims[d].name);
            std::vector<size_t> shape = var_shape(zf, v, nrecs);
            std::string meta;
            dispatch_type(v.type, [&](auto x) {
                using T = decltype(x);
                meta = zarr_chunk::zarr_v3_metadata<T>(shape, chunks, fill_value<T>(v),
                                                       dim_names, atts_json(v.atts));
            });
            fs::path dir = fs::path(zf.path) / v.name;
            fs::create_directories(dir);
            std::ofstream out(dir / "zarr.json");
            out << meta;
            if (!out)
                return PIO_EIO;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "PIO: ERROR: writing Zarr metadata of %s failed: %s\n", zf.path.c_str(),
                e.what());
        return PIO_EIO;
    }
    return PIO_NOERR;
}

/* Agree on the number of records and write the metadata (io tasks). */
int flush_metadata(iosystem_desc_t *ios, ZFile &zf)
{
    unsigned long long local = zf.nrecs, nrecs = 0;
    int mpierr = MPI_Allreduce(&local, &nrecs, 1, MPI_UNSIGNED_LONG_LONG, MPI_MAX, ios->io_comm);
    if (mpierr != MPI_SUCCESS)
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
    zf.nrecs = nrecs;
    int ierr = PIO_NOERR;
    if (ios->io_rank == 0)
        ierr = write_metadata(zf, zf.nrecs);
    MPI_Bcast(&ierr, 1, MPI_INT, 0, ios->io_comm);
    return ierr;
}

std::vector<ZAtt> *att_list(file_desc_t *file, int varid)
{
    ZFile *zf = zfile(file);
    if (!zf)
        return nullptr;
    if (varid == PIO_GLOBAL)
        return &zf->gatts;
    ZVar *v = zvar(file, varid);
    return v ? &v->atts : nullptr;
}

void copy_name(char *dst, const std::string &src)
{
    strncpy(dst, src.c_str(), PIO_MAX_NAME);
    dst[PIO_MAX_NAME] = '\0';
}

} // namespace

int spio_zarr_create(iosystem_desc_t *ios, file_desc_t *file, const char *filename)
{
    assert(ios && file && filename);
    if (ios->async)
        return pio_err(ios, file, PIO_EINVAL, __FILE__, __LINE__,
                       "Creating file (%s) with PIO_IOTYPE_ZARR failed. Async I/O is not supported "
                       "by the Zarr iotype", filename);

    ZFile *zf = new ZFile;
    zf->path = filename;
    file->zarr_file = zf;

    /* io rank 0 makes the (empty) store; the other io tasks wait for it */
    int ierr = PIO_NOERR;
    if (ios->ioproc) {
        if (ios->io_rank == 0) {
            std::error_code ec;
            if (fs::exists(zf->path, ec)) {
                if (file->mode & PIO_NOCLOBBER)
                    ierr = PIO_EEXIST;
                else
                    fs::remove_all(zf->path, ec);
            }
            if (ierr == PIO_NOERR && (!fs::create_directories(zf->path, ec) || ec))
                ierr = PIO_EIO;
        }
        MPI_Bcast(&ierr, 1, MPI_INT, 0, ios->io_comm);
    }
    return ierr;
}

void spio_zarr_free(file_desc_t *file)
{
    delete zfile(file);
    file->zarr_file = NULL;
}

int spio_zarr_def_dim(file_desc_t *file, const char *name, PIO_Offset len, int *idp)
{
    ZFile *zf = zfile(file);
    assert(zf && name);
    for (const ZDim &d : zf->dims)
        if (d.name == name)
            return PIO_ENAMEINUSE;
    zf->dims.push_back(ZDim{name, len});
    if (idp)
        *idp = (int)zf->dims.size() - 1;
    return PIO_NOERR;
}

int spio_zarr_def_var(file_desc_t *file, const char *name, nc_type xtype, int ndims,
                      const int *dimids, int *varidp)
{
    ZFile *zf = zfile(file);
    assert(zf && name);
    if (type_size(xtype) == 0)
        return PIO_EBADTYPE;
    for (const ZVar &v : zf->vars)
        if (v.name == name)
            return PIO_ENAMEINUSE;
    ZVar v;
    v.name = name;
    v.type = xtype;
    for (int i = 0; i < ndims; i++) {
        if (dimids[i] < 0 || dimids[i] >= (int)zf->dims.size())
            return PIO_EBADDIM;
        /* Only the first dimension may be unlimited */
        if (i > 0 && is_unlimited(*zf, dimids[i]))
            return PIO_EUNLIMPOS;
        v.dimids.push_back(dimids[i]);
    }
    zf->vars.push_back(v);
    if (varidp)
        *varidp = (int)zf->vars.size() - 1;
    return PIO_NOERR;
}

int spio_zarr_def_var_chunking(file_desc_t *file, int varid, int storage,
                               const PIO_Offset *chunksizesp)
{
    ZFile *zf = zfile(file);
    ZVar *v = zvar(file, varid);
    if (!v)
        return PIO_ENOTVAR;
    if (storage != NC_CHUNKED) {
        /* "Contiguous": one chunk per record */
        v->chunks.clear();
        for (int d : v->dimids)
            v->chunks.push_back(is_unlimited(*zf, d) ? 1 : (size_t)std::max<PIO_Offset>(1, zf->dims[d].len));
        return PIO_NOERR;
    }
    if (!chunksizesp)
        return PIO_EINVAL;
    v->chunks.clear();
    for (size_t i = 0; i < v->dimids.size(); i++) {
        if (chunksizesp[i] <= 0)
            return PIO_EBADCHUNK;
        /* Each record gets its own chunks so that records written at
         * different times never share a chunk */
        v->chunks.push_back(is_unlimited(*zf, v->dimids[i]) ? 1 : (size_t)chunksizesp[i]);
    }
    return PIO_NOERR;
}

int spio_zarr_put_att(file_desc_t *file, int varid, const char *name, nc_type atttype,
                      PIO_Offset len, nc_type memtype, const void *op)
{
    std::vector<ZAtt> *atts = att_list(file, varid);
    if (!atts)
        return PIO_ENOTVAR;
    ZAtt a{name, atttype, len, ""};
    int ierr = att_json(atttype, len, memtype, op, a.json);
    if (ierr != PIO_NOERR)
        return ierr;
    if (varid != PIO_GLOBAL && a.name == "_FillValue") {
        ZVar *v = zvar(file, varid);
        if (atttype != v->type || len != 1)
            return PIO_EBADTYPE;
        /* The Zarr fill_value; the attribute itself is encoded like xarray
         * does, so xarray applies it as the CF missing value */
        v->fill_json = a.json;
        if (atttype == NC_FLOAT || atttype == NC_DOUBLE)
            dispatch_type(atttype, [&](auto x) {
                a.json = fill_attr_json((double)fill_value<decltype(x)>(*v));
            });
    }
    auto it = std::find_if(atts->begin(), atts->end(),
                           [&](const ZAtt &x) { return x.name == a.name; });
    if (it != atts->end())
        *it = a;
    else
        atts->push_back(a);
    return PIO_NOERR;
}

int spio_zarr_inq(file_desc_t *file, int *ndimsp, int *nvarsp, int *ngattsp, int *unlimdimidp)
{
    ZFile *zf = zfile(file);
    assert(zf);
    if (ndimsp)
        *ndimsp = (int)zf->dims.size();
    if (nvarsp)
        *nvarsp = (int)zf->vars.size();
    if (ngattsp)
        *ngattsp = (int)zf->gatts.size();
    if (unlimdimidp) {
        *unlimdimidp = -1;
        for (size_t i = 0; i < zf->dims.size(); i++)
            if (is_unlimited(*zf, (int)i)) {
                *unlimdimidp = (int)i;
                break;
            }
    }
    return PIO_NOERR;
}

int spio_zarr_inq_dim(file_desc_t *file, int dimid, char *name, PIO_Offset *lenp)
{
    ZFile *zf = zfile(file);
    assert(zf);
    if (dimid < 0 || dimid >= (int)zf->dims.size())
        return PIO_EBADDIM;
    if (name)
        copy_name(name, zf->dims[dimid].name);
    if (lenp)
        *lenp = is_unlimited(*zf, dimid) ? (PIO_Offset)zf->nrecs : zf->dims[dimid].len;
    return PIO_NOERR;
}

int spio_zarr_inq_dimid(file_desc_t *file, const char *name, int *idp)
{
    ZFile *zf = zfile(file);
    assert(zf && name);
    for (size_t i = 0; i < zf->dims.size(); i++)
        if (zf->dims[i].name == name) {
            if (idp)
                *idp = (int)i;
            return PIO_NOERR;
        }
    return PIO_EBADDIM;
}

int spio_zarr_inq_var(file_desc_t *file, int varid, char *name, nc_type *xtypep, int *ndimsp,
                      int *dimidsp, int *nattsp)
{
    ZVar *v = zvar(file, varid);
    if (!v)
        return PIO_ENOTVAR;
    if (name)
        copy_name(name, v->name);
    if (xtypep)
        *xtypep = v->type;
    if (ndimsp)
        *ndimsp = (int)v->dimids.size();
    if (dimidsp)
        std::copy(v->dimids.begin(), v->dimids.end(), dimidsp);
    if (nattsp)
        *nattsp = (int)v->atts.size();
    return PIO_NOERR;
}

int spio_zarr_inq_varid(file_desc_t *file, const char *name, int *varidp)
{
    ZFile *zf = zfile(file);
    assert(zf && name);
    for (size_t i = 0; i < zf->vars.size(); i++)
        if (zf->vars[i].name == name) {
            if (varidp)
                *varidp = (int)i;
            return PIO_NOERR;
        }
    return PIO_ENOTVAR;
}

int spio_zarr_inq_var_chunking(file_desc_t *file, int varid, int *storagep,
                               PIO_Offset *chunksizesp)
{
    ZFile *zf = zfile(file);
    ZVar *v = zvar(file, varid);
    if (!v)
        return PIO_ENOTVAR;
    if (storagep)
        *storagep = NC_CHUNKED;
    if (chunksizesp) {
        std::vector<size_t> chunks = v->chunks.empty() ? guess_chunks(*zf, *v, type_size(v->type))
                                                       : v->chunks;
        for (size_t i = 0; i < chunks.size(); i++)
            chunksizesp[i] = (PIO_Offset)chunks[i];
    }
    return PIO_NOERR;
}

int spio_zarr_inq_var_fill(file_desc_t *file, int varid, int *no_fill, void *fill_valuep)
{
    ZVar *v = zvar(file, varid);
    if (!v)
        return PIO_ENOTVAR;
    /* Chunks never written are read as the fill value: always fill mode */
    if (no_fill)
        *no_fill = 0;
    if (fill_valuep)
        dispatch_type(v->type, [&](auto x) {
            using T = decltype(x);
            T fill = fill_value<T>(*v);
            memcpy(fill_valuep, &fill, sizeof(T));
        });
    return PIO_NOERR;
}

int spio_zarr_inq_att(file_desc_t *file, int varid, const char *name, nc_type *xtypep,
                      PIO_Offset *lenp)
{
    std::vector<ZAtt> *atts = att_list(file, varid);
    if (!atts)
        return PIO_ENOTVAR;
    for (const ZAtt &a : *atts)
        if (a.name == name) {
            if (xtypep)
                *xtypep = a.type;
            if (lenp)
                *lenp = a.len;
            return PIO_NOERR;
        }
    return PIO_ENOTATT;
}

int spio_zarr_inq_attname(file_desc_t *file, int varid, int attnum, char *name)
{
    std::vector<ZAtt> *atts = att_list(file, varid);
    if (!atts)
        return PIO_ENOTVAR;
    if (attnum < 0 || attnum >= (int)atts->size())
        return PIO_ENOTATT;
    if (name)
        copy_name(name, (*atts)[attnum].name);
    return PIO_NOERR;
}

int spio_zarr_inq_attid(file_desc_t *file, int varid, const char *name, int *idp)
{
    std::vector<ZAtt> *atts = att_list(file, varid);
    if (!atts)
        return PIO_ENOTVAR;
    for (size_t i = 0; i < atts->size(); i++)
        if ((*atts)[i].name == name) {
            if (idp)
                *idp = (int)i;
            return PIO_NOERR;
        }
    return PIO_ENOTATT;
}

int spio_zarr_write_darray(iosystem_desc_t *ios, file_desc_t *file, int varid,
                           const io_desc_t *iodesc, int fndims, int nblocks,
                           const PIO_Offset *starts, const PIO_Offset *counts,
                           const void *const *bufs)
{
    ZFile *zf = zfile(file);
    ZVar *v = zvar(file, varid);
    assert(ios && ios->ioproc && iodesc);
    if (!v)
        return PIO_ENOTVAR;
    if (fndims != (int)v->dimids.size())
        return PIO_EINVAL;

    /* Chunks of a distributed array default to the ones the decomposition
     * was split into (same on all io tasks) */
    if (v->chunks.empty())
        v->chunks = guess_chunks(*zf, *v, iodesc->mpitype_size, iodesc->dimlen, iodesc->ndims);

    std::vector<zarr_chunk::Box> boxes(nblocks);
    for (int b = 0; b < nblocks; b++) {
        boxes[b].start.assign(starts + (size_t)b * fndims, starts + (size_t)(b + 1) * fndims);
        boxes[b].count.assign(counts + (size_t)b * fndims, counts + (size_t)(b + 1) * fndims);
        if (fndims > 0 && is_unlimited(*zf, v->dimids[0]) && !zarr_chunk::box_is_empty(boxes[b]))
            zf->nrecs = std::max(zf->nrecs, boxes[b].start[0] + boxes[b].count[0]);
    }

    /* The unlimited dimension extent must be the same on all io tasks */
    unsigned long long local = zf->nrecs, nrecs = 0;
    int mpierr = MPI_Allreduce(&local, &nrecs, 1, MPI_UNSIGNED_LONG_LONG, MPI_MAX, ios->io_comm);
    if (mpierr != MPI_SUCCESS)
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
    zf->nrecs = nrecs;
    std::vector<size_t> shape = var_shape(*zf, *v, zf->nrecs);

    int ierr = PIO_NOERR;
    try {
        ierr = dispatch_type(v->type, [&](auto x) {
            using T = decltype(x);
            std::vector<std::vector<T>> data(nblocks);
            std::vector<const T *> ptrs(nblocks);
            for (int b = 0; b < nblocks; b++) {
                if (convert(iodesc->piotype, bufs[b], zarr_chunk::box_size(boxes[b]), data[b]) !=
                    PIO_NOERR)
                    throw std::invalid_argument("unsupported memory type");
                ptrs[b] = data[b].data();
            }
            std::string dir = (fs::path(zf->path) / v->name).string();
            for (const zarr_chunk::Chunk &c :
                 zarr_chunk::chunk_blocks_mpi<T>(ios->io_comm, ptrs, boxes, shape, v->chunks,
                                                 fill_value<T>(*v)))
                zarr_chunk::write_chunk_file(dir, c);
        });
    } catch (const std::exception &e) {
        fprintf(stderr, "PIO: ERROR: writing variable %s to Zarr store %s failed: %s\n",
                v->name.c_str(), zf->path.c_str(), e.what());
        ierr = PIO_EIO;
    }
    return ierr;
}

int spio_zarr_put_var(iosystem_desc_t *ios, file_desc_t *file, int varid,
                      const PIO_Offset *start, const PIO_Offset *count,
                      const PIO_Offset *stride, nc_type memtype, const void *buf)
{
    ZFile *zf = zfile(file);
    ZVar *v = zvar(file, varid);
    assert(ios && ios->ioproc);
    if (!v)
        return PIO_ENOTVAR;
    const size_t nd = v->dimids.size();
    for (size_t i = 0; stride && i < nd; i++)
        if (stride[i] != 1)
            return PIO_EINVAL;

    zarr_chunk::Box box;
    for (size_t i = 0; i < nd; i++) {
        box.start.push_back(start ? (size_t)start[i] : 0);
        box.count.push_back(count ? (size_t)count[i] : 1);
    }
    if (nd > 0 && is_unlimited(*zf, v->dimids[0]) && box.count[0] > 0)
        zf->nrecs = std::max(zf->nrecs, box.start[0] + box.count[0]);
    if (v->chunks.empty())
        v->chunks = guess_chunks(*zf, *v, type_size(v->type));

    /* Only io rank 0 writes. The chunks it touches may hold data written
     * earlier, so they are read, updated and written back. */
    if (ios->io_rank != 0 || zarr_chunk::box_is_empty(box))
        return PIO_NOERR;

    std::vector<size_t> shape = var_shape(*zf, *v, zf->nrecs);
    int ierr = PIO_NOERR;
    try {
        ierr = dispatch_type(v->type, [&](auto x) {
            using T = decltype(x);
            std::vector<T> data;
            if (convert(memtype, buf, zarr_chunk::box_size(box), data) != PIO_NOERR)
                throw std::invalid_argument("unsupported memory type");
            const size_t chunk_len = zarr_chunk::box_size(zarr_chunk::Box{{}, v->chunks});
            fs::path dir = fs::path(zf->path) / v->name;

            std::vector<size_t> lo, hi;
            zarr_chunk::detail::chunk_range(box, v->chunks, lo, hi);
            zarr_chunk::detail::for_each_chunk(lo, hi, [&](const std::vector<size_t> &index) {
                zarr_chunk::Box region, cbox;
                if (!zarr_chunk::detail::intersect(box, v->chunks, shape, index, region))
                    return;
                cbox.count = v->chunks;
                for (size_t d = 0; d < nd; d++)
                    cbox.start.push_back(index[d] * v->chunks[d]);

                zarr_chunk::Chunk c;
                c.index = index;
                c.key = zarr_chunk::chunk_key(index, zarr_chunk::KeyEncoding::V3Default);
                std::vector<T> cbuf(chunk_len, fill_value<T>(*v));
                std::ifstream in(dir / c.key, std::ios::binary);
                if (in)
                    in.read(reinterpret_cast<char *>(cbuf.data()), chunk_len * sizeof(T));
                zarr_chunk::detail::copy_region(data.data(), box, cbuf.data(), cbox, region);
                c.bytes.resize(chunk_len * sizeof(T));
                memcpy(c.bytes.data(), cbuf.data(), c.bytes.size());
                zarr_chunk::write_chunk_file(dir.string(), c);
            });
        });
    } catch (const std::exception &e) {
        fprintf(stderr, "PIO: ERROR: writing variable %s to Zarr store %s failed: %s\n",
                v->name.c_str(), zf->path.c_str(), e.what());
        ierr = PIO_EIO;
    }
    return ierr;
}

int spio_zarr_enddef(iosystem_desc_t *ios, file_desc_t *file)
{
    assert(ios && ios->ioproc && zfile(file));
    return flush_metadata(ios, *zfile(file));
}

int spio_zarr_sync(iosystem_desc_t *ios, file_desc_t *file)
{
    assert(ios && ios->ioproc && zfile(file));
    return flush_metadata(ios, *zfile(file));
}

int spio_zarr_close(iosystem_desc_t *ios, file_desc_t *file)
{
    assert(ios && ios->ioproc && zfile(file));
    int ierr = flush_metadata(ios, *zfile(file));
    /* All chunks are on disk when close returns */
    MPI_Barrier(ios->io_comm);
    return ierr;
}

#endif /* _SPIO_ZARR */
