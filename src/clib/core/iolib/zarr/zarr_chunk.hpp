// zarr_chunk.hpp: split an N-D array into chunks the way zarr-python does.
//
// Zarr rules followed here (zarr v3 "regular" chunk grid, "bytes" codec):
//  - The array of shape S is cut by a chunk shape C into
//    ceil(S[d] / C[d]) chunks along each dimension d.
//  - Every chunk is stored with the full chunk shape C, in C (row-major)
//    order. Edge chunks that stick out past the array are padded with the
//    fill value.
//  - Chunk keys: v3 default encoding "c/i/j/k"; v2 encoding "i.j.k".
//  - Chunks whose values are all the fill value are not written unless
//    write_empty_chunks is true (zarr-python's default is false).
//  - Values are stored little-endian; this code assumes a little-endian host.
//
// No compression is applied (codecs = [bytes]); zarr-python reads such
// chunks directly.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace zarr_chunk {

enum class KeyEncoding { V3Default, V2 };

struct Chunk {
    std::vector<size_t> index;  // chunk position in the chunk grid
    std::string key;            // e.g. "c/0/1" (v3) or "0.1" (v2)
    std::vector<uint8_t> bytes; // prod(chunk_shape) values, C order
};

// Number of chunks along each dimension: ceil(shape[d] / chunks[d]).
inline std::vector<size_t> chunk_grid(const std::vector<size_t>& shape,
                                      const std::vector<size_t>& chunks)
{
    if (shape.size() != chunks.size())
        throw std::invalid_argument("shape and chunks have different ranks");
    std::vector<size_t> grid(shape.size());
    for (size_t d = 0; d < shape.size(); d++) {
        if (chunks[d] == 0)
            throw std::invalid_argument("chunk size must be > 0");
        grid[d] = (shape[d] + chunks[d] - 1) / chunks[d];
    }
    return grid;
}

inline std::string chunk_key(const std::vector<size_t>& index, KeyEncoding enc)
{
    std::string key = (enc == KeyEncoding::V3Default) ? "c" : "";
    const char sep = (enc == KeyEncoding::V3Default) ? '/' : '.';
    for (size_t d = 0; d < index.size(); d++) {
        if (enc == KeyEncoding::V3Default || d > 0)
            key += sep;
        key += std::to_string(index[d]);
    }
    if (key.empty()) // 0-d array in v2
        key = "0";
    return key;
}

namespace detail {

// Advance a C-order multi-index within extents; false when it wraps around.
inline bool next_index(std::vector<size_t>& idx, const std::vector<size_t>& ext)
{
    for (size_t d = idx.size(); d-- > 0;) {
        if (++idx[d] < ext[d])
            return true;
        idx[d] = 0;
    }
    return false;
}

template <typename T>
bool same_value(T a, T b)
{
    if constexpr (std::is_floating_point_v<T>)
        if (std::isnan(a) && std::isnan(b))
            return true;
    return a == b;
}

} // namespace detail

// Copy the chunk at grid position `index` out of `data` (shape `shape`,
// C order) into a full chunk-shaped buffer padded with `fill_value`.
template <typename T>
std::vector<T> extract_chunk(const T* data, const std::vector<size_t>& shape,
                             const std::vector<size_t>& chunks, const std::vector<size_t>& index,
                             T fill_value)
{
    const size_t ndims = shape.size();
    size_t chunk_len = 1;
    for (size_t c : chunks)
        chunk_len *= c;
    std::vector<T> out(chunk_len, fill_value);
    if (ndims == 0) {
        out[0] = data[0];
        return out;
    }

    // Start of the chunk in the array, and the part of it inside the array.
    std::vector<size_t> start(ndims), valid(ndims);
    for (size_t d = 0; d < ndims; d++) {
        start[d] = index[d] * chunks[d];
        valid[d] = std::min(chunks[d], shape[d] - start[d]);
    }

    // Strides (in elements) of the array and of the chunk buffer.
    std::vector<size_t> astride(ndims, 1), cstride(ndims, 1);
    for (size_t d = ndims - 1; d-- > 0;) {
        astride[d] = astride[d + 1] * shape[d + 1];
        cstride[d] = cstride[d + 1] * chunks[d + 1];
    }

    // Copy contiguous runs along the last dimension, one per position of
    // the leading dimensions of the valid region.
    std::vector<size_t> lead(valid.begin(), valid.end() - 1);
    std::vector<size_t> pos(ndims - 1, 0);
    const size_t run = valid[ndims - 1];
    do {
        size_t src = start[ndims - 1], dst = 0;
        for (size_t d = 0; d + 1 < ndims; d++) {
            src += (start[d] + pos[d]) * astride[d];
            dst += pos[d] * cstride[d];
        }
        std::memcpy(&out[dst], &data[src], run * sizeof(T));
    } while (detail::next_index(pos, lead));
    return out;
}

// A box of the global array: offsets `start` and sizes `count` per
// dimension. Data held for a box is stored in C order.
struct Box {
    std::vector<size_t> start, count;
};

inline bool box_is_empty(const Box& b)
{
    for (size_t c : b.count)
        if (c == 0)
            return true;
    return false;
}

inline size_t box_size(const Box& b)
{
    size_t n = 1;
    for (size_t c : b.count)
        n *= c;
    return n;
}

namespace detail {

// Intersection of box b with the chunk at grid position `index`.
// Returns false if they do not overlap.
inline bool intersect(const Box& b, const std::vector<size_t>& chunks,
                      const std::vector<size_t>& shape, const std::vector<size_t>& index, Box& out)
{
    const size_t nd = chunks.size();
    out.start.resize(nd);
    out.count.resize(nd);
    for (size_t d = 0; d < nd; d++) {
        size_t c0 = index[d] * chunks[d];
        size_t c1 = std::min(c0 + chunks[d], shape[d]);
        size_t lo = std::max(c0, b.start[d]);
        size_t hi = std::min(c1, b.start[d] + b.count[d]);
        if (lo >= hi)
            return false;
        out.start[d] = lo;
        out.count[d] = hi - lo;
    }
    return true;
}

// Range of chunk indices [lo, hi) touched by box b along each dimension.
inline void chunk_range(const Box& b, const std::vector<size_t>& chunks, std::vector<size_t>& lo,
                        std::vector<size_t>& hi)
{
    const size_t nd = chunks.size();
    lo.resize(nd);
    hi.resize(nd);
    for (size_t d = 0; d < nd; d++) {
        lo[d] = b.start[d] / chunks[d];
        hi[d] = (b.start[d] + b.count[d] + chunks[d] - 1) / chunks[d];
    }
}

// Visit chunk indices in [lo, hi) in C order.
template <typename F>
void for_each_chunk(const std::vector<size_t>& lo, const std::vector<size_t>& hi, F f)
{
    const size_t nd = lo.size();
    std::vector<size_t> ext(nd), off(nd, 0), index(lo);
    for (size_t d = 0; d < nd; d++) {
        if (hi[d] <= lo[d])
            return;
        ext[d] = hi[d] - lo[d];
    }
    do {
        for (size_t d = 0; d < nd; d++)
            index[d] = lo[d] + off[d];
        f(index);
    } while (next_index(off, ext));
}

// C-order position of a chunk in the chunk grid.
inline size_t linear_index(const std::vector<size_t>& index, const std::vector<size_t>& grid)
{
    size_t l = 0;
    for (size_t d = 0; d < grid.size(); d++)
        l = l * grid[d] + index[d];
    return l;
}

// Copy box `region` (global coordinates) from an array laid out as box
// `src_box` into an array laid out as box `dst_box`, both in C order.
template <typename T>
void copy_region(const T* src, const Box& src_box, T* dst, const Box& dst_box, const Box& region)
{
    const size_t nd = region.count.size();
    if (nd == 0) {
        dst[0] = src[0];
        return;
    }
    std::vector<size_t> sstride(nd, 1), dstride(nd, 1);
    for (size_t d = nd - 1; d-- > 0;) {
        sstride[d] = sstride[d + 1] * src_box.count[d + 1];
        dstride[d] = dstride[d + 1] * dst_box.count[d + 1];
    }
    std::vector<size_t> lead(region.count.begin(), region.count.end() - 1);
    std::vector<size_t> pos(nd - 1, 0);
    const size_t run = region.count[nd - 1];
    do {
        size_t s = region.start[nd - 1] - src_box.start[nd - 1];
        size_t t = region.start[nd - 1] - dst_box.start[nd - 1];
        for (size_t d = 0; d + 1 < nd; d++) {
            s += (region.start[d] + pos[d] - src_box.start[d]) * sstride[d];
            t += (region.start[d] + pos[d] - dst_box.start[d]) * dstride[d];
        }
        std::memcpy(dst + t, src + s, run * sizeof(T));
    } while (next_index(pos, lead));
}

} // namespace detail

// Split the whole array into Zarr chunks.
template <typename T>
std::vector<Chunk>
chunk_array(const T* data, const std::vector<size_t>& shape, const std::vector<size_t>& chunks,
            T fill_value, bool write_empty_chunks = false, KeyEncoding enc = KeyEncoding::V3Default)
{
    static_assert(std::is_arithmetic_v<T>, "chunk_array needs a numeric type");
    const std::vector<size_t> grid = chunk_grid(shape, chunks);
    for (size_t g : grid)
        if (g == 0) // an empty array has no chunks
            return {};

    std::vector<Chunk> result;
    std::vector<size_t> index(shape.size(), 0);
    do {
        std::vector<T> buf = extract_chunk(data, shape, chunks, index, fill_value);
        if (!write_empty_chunks) {
            bool empty = true;
            for (const T& v : buf)
                if (!detail::same_value(v, fill_value)) {
                    empty = false;
                    break;
                }
            if (empty)
                continue;
        }
        Chunk c;
        c.index = index;
        c.key = chunk_key(index, enc);
        c.bytes.resize(buf.size() * sizeof(T));
        std::memcpy(c.bytes.data(), buf.data(), c.bytes.size());
        result.push_back(std::move(c));
    } while (detail::next_index(index, grid));
    return result;
}

// Chunks touched by a set of blocks of the array, all held by this process:
// blocks[i] holds box boxes[i] in C order. Parts of a chunk that no block
// covers get the fill value; where blocks overlap, the later one wins.
// Chunks that no block touches are not returned. The result is the whole
// array only if every chunk's data is among the blocks (always true for a
// single writer; see zarr_chunk_mpi.hpp for blocks spread over ranks).
template <typename T>
std::vector<Chunk> chunk_blocks(const std::vector<const T*>& blocks, const std::vector<Box>& boxes,
                                const std::vector<size_t>& shape, const std::vector<size_t>& chunks,
                                T fill_value, bool write_empty_chunks = false,
                                KeyEncoding enc = KeyEncoding::V3Default)
{
    static_assert(std::is_arithmetic_v<T>, "chunk_blocks needs a numeric type");
    const size_t nd = shape.size();
    if (blocks.size() != boxes.size())
        throw std::invalid_argument("blocks and boxes have different lengths");
    const std::vector<size_t> grid = chunk_grid(shape, chunks);
    for (size_t g : grid)
        if (g == 0)
            return {};

    // Chunks touched by any block, in C order of the chunk grid.
    std::map<size_t, std::vector<size_t>> touched;
    for (const Box& b : boxes) {
        if (b.start.size() != nd || b.count.size() != nd)
            throw std::invalid_argument("box rank differs from array rank");
        if (box_is_empty(b))
            continue;
        std::vector<size_t> lo, hi;
        detail::chunk_range(b, chunks, lo, hi);
        detail::for_each_chunk(lo, hi, [&](const std::vector<size_t>& index) {
            touched.emplace(detail::linear_index(index, grid), index);
        });
    }

    const size_t chunk_len = box_size(Box{{}, chunks});
    std::vector<Chunk> result;
    for (const auto& [linear, index] : touched) {
        Box cbox; // the full (unclipped) chunk, as the destination layout
        cbox.count = chunks;
        cbox.start.resize(nd);
        for (size_t d = 0; d < nd; d++)
            cbox.start[d] = index[d] * chunks[d];
        std::vector<T> buf(chunk_len, fill_value);
        for (size_t i = 0; i < boxes.size(); i++) {
            Box piece;
            if (!box_is_empty(boxes[i]) && detail::intersect(boxes[i], chunks, shape, index, piece))
                detail::copy_region(blocks[i], boxes[i], buf.data(), cbox, piece);
        }
        if (!write_empty_chunks && std::all_of(buf.begin(), buf.end(), [&](T v) {
                return detail::same_value(v, fill_value);
            }))
            continue;
        Chunk c;
        c.index = index;
        c.key = chunk_key(index, enc);
        c.bytes.resize(buf.size() * sizeof(T));
        std::memcpy(c.bytes.data(), buf.data(), c.bytes.size());
        result.push_back(std::move(c));
    }
    return result;
}

// Zarr v3 data type names.
template <typename T>
constexpr const char* zarr_dtype();
template <>
constexpr const char* zarr_dtype<int8_t>()
{
    return "int8";
}
template <>
constexpr const char* zarr_dtype<int16_t>()
{
    return "int16";
}
template <>
constexpr const char* zarr_dtype<int32_t>()
{
    return "int32";
}
template <>
constexpr const char* zarr_dtype<int64_t>()
{
    return "int64";
}
template <>
constexpr const char* zarr_dtype<uint8_t>()
{
    return "uint8";
}
template <>
constexpr const char* zarr_dtype<uint16_t>()
{
    return "uint16";
}
template <>
constexpr const char* zarr_dtype<uint32_t>()
{
    return "uint32";
}
template <>
constexpr const char* zarr_dtype<uint64_t>()
{
    return "uint64";
}
template <>
constexpr const char* zarr_dtype<float>()
{
    return "float32";
}
template <>
constexpr const char* zarr_dtype<double>()
{
    return "float64";
}

namespace detail {

template <typename T>
std::string json_number(T v)
{
    if constexpr (std::is_floating_point_v<T>) {
        if (std::isnan(v))
            return "\"NaN\"";
        if (std::isinf(v))
            return v > 0 ? "\"Infinity\"" : "\"-Infinity\"";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", static_cast<double>(v));
        // A float must look like one ("-999.0", not "-999"): zarr-python
        // rejects an integer fill_value for a float data type
        std::string s = buf;
        if (s.find_first_of(".eE") == std::string::npos)
            s += ".0";
        return s;
    } else {
        return std::to_string(v);
    }
}

inline std::string json_list(const std::vector<size_t>& v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
}

} // namespace detail

// Contents of zarr.json for an uncompressed Zarr v3 array. `attributes` is
// a JSON object.
template <typename T>
std::string zarr_v3_metadata(const std::vector<size_t>& shape, const std::vector<size_t>& chunks,
                             T fill_value, const std::vector<std::string>& dim_names = {},
                             const std::string& attributes = "{}")
{
    std::string meta = "{\n  \"zarr_format\": 3,\n  \"node_type\": \"array\",\n";
    meta += "  \"shape\": " + detail::json_list(shape) + ",\n";
    meta += std::string("  \"data_type\": \"") + zarr_dtype<T>() + "\",\n";
    meta += "  \"chunk_grid\": {\"name\": \"regular\", \"configuration\": "
            "{\"chunk_shape\": " +
            detail::json_list(chunks) + "}},\n";
    meta += "  \"chunk_key_encoding\": {\"name\": \"default\", "
            "\"configuration\": {\"separator\": \"/\"}},\n";
    meta += "  \"fill_value\": " + detail::json_number(fill_value) + ",\n";
    meta += "  \"codecs\": [{\"name\": \"bytes\", \"configuration\": "
            "{\"endian\": \"little\"}}],\n";
    if (!dim_names.empty()) {
        meta += "  \"dimension_names\": [";
        for (size_t i = 0; i < dim_names.size(); i++)
            meta += (i ? ", \"" : "\"") + dim_names[i] + "\"";
        meta += "],\n";
    }
    meta += "  \"attributes\": " + attributes + "\n}\n";
    return meta;
}

// Write one chunk file at <path>/<key>, creating directories as needed.
// Safe to call from several processes at once on a shared file system.
inline void write_chunk_file(const std::string& path, const Chunk& c)
{
    namespace fs = std::filesystem;
    fs::path file = fs::path(path) / c.key;
    std::error_code ec; // another process may create the same directory
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(c.bytes.data()),
              static_cast<std::streamsize>(c.bytes.size()));
    if (!out)
        throw std::runtime_error("cannot write chunk file " + file.string());
}

// Write the array as a Zarr v3 array directory: <path>/zarr.json plus one
// file per chunk at <path>/c/i/j/... . dim_names may be empty.
template <typename T>
void write_zarr_v3(const std::string& path, const T* data, const std::vector<size_t>& shape,
                   const std::vector<size_t>& chunks, T fill_value,
                   const std::vector<std::string>& dim_names = {}, bool write_empty_chunks = false)
{
    namespace fs = std::filesystem;
    fs::create_directories(path);
    std::ofstream(fs::path(path) / "zarr.json")
        << zarr_v3_metadata(shape, chunks, fill_value, dim_names);

    for (const Chunk& c : chunk_array(data, shape, chunks, fill_value, write_empty_chunks))
        write_chunk_file(path, c);
}

} // namespace zarr_chunk
