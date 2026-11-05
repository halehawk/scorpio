// zarr_chunk_mpi.hpp: Zarr chunking of a distributed array with MPI.
//
// Each rank holds zero or more boxes (blocks) of the global array: offsets
// `start` and sizes `count` per dimension, each stored in C order. Parts of
// the array that no rank holds are filled with the fill value, as in
// zarr-python. Boxes should not overlap; where they do, the box of the
// higher rank (or the later box on one rank) wins.
//
// Each Zarr chunk gets one owner rank, which assembles the full chunk
// (padded with the fill value, same as zarr_chunk.hpp) and writes it.
// The owner is the rank holding the chunk's first element if any, else the
// lowest rank holding any part of the chunk. So when the boxes are aligned
// to chunk boundaries every rank owns exactly the chunks in its boxes and no
// data moves; otherwise the pieces are exchanged with one MPI_Alltoallv.
//
// All ranks must call these functions collectively with the same shape,
// chunks and fill value.

#pragma once

#include "zarr_chunk.hpp"

#include <mpi.h>

#include <algorithm>
#include <climits>
#include <map>
#include <tuple>

namespace zarr_chunk {

namespace detail {

inline int to_int(size_t n)
{
    if (n > static_cast<size_t>(INT_MAX))
        throw std::overflow_error("zarr_chunk_mpi: more than INT_MAX bytes "
                                  "exchanged with one rank");
    return static_cast<int>(n);
}

} // namespace detail

// Assemble the Zarr chunks owned by this rank, from the blocks every rank
// holds. blocks[i] holds box boxes[i] of the global array in C order.
template <typename T>
std::vector<Chunk>
chunk_blocks_mpi(MPI_Comm comm, const std::vector<const T*>& blocks, const std::vector<Box>& boxes,
                 const std::vector<size_t>& shape, const std::vector<size_t>& chunks, T fill_value,
                 bool write_empty_chunks = false, KeyEncoding enc = KeyEncoding::V3Default)
{
    static_assert(std::is_arithmetic_v<T>, "chunk_blocks_mpi needs a numeric type");
    int rank, nprocs;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &nprocs);
    const size_t nd = shape.size();
    if (blocks.size() != boxes.size())
        throw std::invalid_argument("blocks and boxes have different lengths");
    for (const Box& b : boxes)
        if (b.start.size() != nd || b.count.size() != nd)
            throw std::invalid_argument("box rank differs from array rank");
    const std::vector<size_t> grid = chunk_grid(shape, chunks);
    for (size_t g : grid)
        if (g == 0)
            return {};

    // My non-empty blocks.
    std::vector<size_t> my;
    for (size_t i = 0; i < boxes.size(); i++)
        if (!box_is_empty(boxes[i]))
            my.push_back(i);

    // Everybody's boxes, ordered by rank and then by block.
    const int width = static_cast<int>(2 * nd);
    int nmine = static_cast<int>(my.size());
    std::vector<int> nboxes(nprocs), displs(nprocs), counts(nprocs);
    MPI_Allgather(&nmine, 1, MPI_INT, nboxes.data(), 1, MPI_INT, comm);
    int total = 0;
    for (int r = 0; r < nprocs; r++) {
        counts[r] = nboxes[r] * width;
        displs[r] = total * width;
        total += nboxes[r];
    }
    std::vector<unsigned long long> mine(my.size() * width), all(size_t(total) * width);
    for (size_t k = 0; k < my.size(); k++)
        for (size_t d = 0; d < nd; d++) {
            mine[k * width + d] = boxes[my[k]].start[d];
            mine[k * width + nd + d] = boxes[my[k]].count[d];
        }
    MPI_Allgatherv(mine.data(), nmine * width, MPI_UNSIGNED_LONG_LONG, all.data(), counts.data(),
                   displs.data(), MPI_UNSIGNED_LONG_LONG, comm);
    std::vector<Box> gbox(total);
    std::vector<int> grank(total);
    for (int r = 0, k = 0; r < nprocs; r++)
        for (int i = 0; i < nboxes[r]; i++, k++) {
            auto at = all.begin() + size_t(k) * width;
            gbox[k].start.assign(at, at + nd);
            gbox[k].count.assign(at + nd, at + width);
            grank[k] = r;
        }

    // Owner of a chunk: the rank holding its first element, else the lowest
    // rank holding any of it, else (nobody) -1.
    auto owner_of = [&](const std::vector<size_t>& index) {
        Box piece;
        int first = -1;
        for (int k = 0; k < total; k++) {
            if (!detail::intersect(gbox[k], chunks, shape, index, piece))
                continue;
            bool has_corner = true;
            for (size_t d = 0; d < nd; d++)
                has_corner &= piece.start[d] == index[d] * chunks[d];
            if (has_corner)
                return grank[k];
            if (first < 0)
                first = grank[k];
        }
        return first;
    };

    // My pieces, ordered by (owner, chunk, block); also the chunks I own,
    // which all lie in my boxes since an owner holds part of its chunk.
    struct Piece {
        int owner;
        size_t linear, block;
        Box region;
    };
    std::vector<Piece> pieces;
    std::map<size_t, std::vector<size_t>> owned; // linear index -> index
    for (size_t k = 0; k < my.size(); k++) {
        std::vector<size_t> lo, hi;
        detail::chunk_range(boxes[my[k]], chunks, lo, hi);
        detail::for_each_chunk(lo, hi, [&](const std::vector<size_t>& index) {
            Piece p;
            if (!detail::intersect(boxes[my[k]], chunks, shape, index, p.region))
                return;
            p.owner = owner_of(index);
            p.linear = detail::linear_index(index, grid);
            p.block = k;
            if (p.owner == rank)
                owned.emplace(p.linear, index);
            pieces.push_back(std::move(p));
        });
    }
    std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        return std::tie(a.owner, a.linear, a.block) < std::tie(b.owner, b.linear, b.block);
    });

    std::vector<T> sendbuf;
    std::vector<size_t> send_elems(nprocs, 0);
    for (const Piece& p : pieces) {
        size_t at = sendbuf.size(), n = box_size(p.region);
        sendbuf.resize(at + n);
        detail::copy_region(blocks[my[p.block]], boxes[my[p.block]], sendbuf.data() + at, p.region,
                            p.region);
        send_elems[p.owner] += n;
    }

    // What each rank sends me: its pieces of my chunks, in the same
    // (chunk, block) order.
    std::vector<size_t> recv_elems(nprocs, 0);
    for (const auto& [linear, index] : owned)
        for (int k = 0; k < total; k++) {
            Box piece;
            if (detail::intersect(gbox[k], chunks, shape, index, piece))
                recv_elems[grank[k]] += box_size(piece);
        }

    std::vector<int> scounts(nprocs), sdispls(nprocs), rcounts(nprocs), rdispls(nprocs);
    size_t soff = 0, roff = 0;
    for (int r = 0; r < nprocs; r++) {
        scounts[r] = detail::to_int(send_elems[r] * sizeof(T));
        sdispls[r] = detail::to_int(soff * sizeof(T));
        soff += send_elems[r];
        rcounts[r] = detail::to_int(recv_elems[r] * sizeof(T));
        rdispls[r] = detail::to_int(roff * sizeof(T));
        roff += recv_elems[r];
    }
    std::vector<T> recvbuf(roff);
    MPI_Alltoallv(sendbuf.data(), scounts.data(), sdispls.data(), MPI_BYTE, recvbuf.data(),
                  rcounts.data(), rdispls.data(), MPI_BYTE, comm);

    // Assemble my chunks.
    const size_t chunk_len = box_size(Box{{}, chunks});
    std::vector<size_t> rpos(nprocs);
    for (int r = 0; r < nprocs; r++)
        rpos[r] = rdispls[r] / sizeof(T);

    auto make_chunk = [&](const std::vector<size_t>& index, const std::vector<T>& buf) {
        Chunk c;
        c.index = index;
        c.key = chunk_key(index, enc);
        c.bytes.resize(buf.size() * sizeof(T));
        std::memcpy(c.bytes.data(), buf.data(), c.bytes.size());
        return c;
    };

    std::vector<Chunk> result;
    for (const auto& [linear, index] : owned) {
        Box cbox; // the full (unclipped) chunk, as the destination layout
        cbox.count = chunks;
        cbox.start.resize(nd);
        for (size_t d = 0; d < nd; d++)
            cbox.start[d] = index[d] * chunks[d];
        std::vector<T> buf(chunk_len, fill_value);
        for (int k = 0; k < total; k++) {
            Box piece;
            if (!detail::intersect(gbox[k], chunks, shape, index, piece))
                continue;
            detail::copy_region(recvbuf.data() + rpos[grank[k]], piece, buf.data(), cbox, piece);
            rpos[grank[k]] += box_size(piece);
        }
        if (!write_empty_chunks && std::all_of(buf.begin(), buf.end(), [&](T v) {
                return detail::same_value(v, fill_value);
            }))
            continue;
        result.push_back(make_chunk(index, buf));
    }

    // Chunks no rank holds any data of are all fill; write them only when
    // asked to, spread round-robin over the ranks.
    if (write_empty_chunks) {
        std::vector<size_t> zero(nd, 0);
        detail::for_each_chunk(zero, grid, [&](const std::vector<size_t>& index) {
            if (detail::linear_index(index, grid) % nprocs == static_cast<size_t>(rank) &&
                owner_of(index) < 0)
                result.push_back(make_chunk(index, std::vector<T>(chunk_len, fill_value)));
        });
    }
    return result;
}

// One box per rank (count may be all zeros for a rank with no data).
template <typename T>
std::vector<Chunk>
chunk_array_mpi(MPI_Comm comm, const T* local, const Box& mybox, const std::vector<size_t>& shape,
                const std::vector<size_t>& chunks, T fill_value, bool write_empty_chunks = false,
                KeyEncoding enc = KeyEncoding::V3Default)
{
    return chunk_blocks_mpi<T>(comm, {local}, {mybox}, shape, chunks, fill_value,
                               write_empty_chunks, enc);
}

// Write a distributed array as a Zarr v3 array directory: rank 0 writes
// zarr.json, every rank writes the chunks it owns. `path` must be on a file
// system all ranks share.
template <typename T>
void write_zarr_v3_mpi(MPI_Comm comm, const std::string& path, const T* local, const Box& mybox,
                       const std::vector<size_t>& shape, const std::vector<size_t>& chunks,
                       T fill_value, const std::vector<std::string>& dim_names = {},
                       bool write_empty_chunks = false)
{
    namespace fs = std::filesystem;
    int rank;
    MPI_Comm_rank(comm, &rank);
    if (rank == 0) {
        fs::create_directories(path);
        std::ofstream(fs::path(path) / "zarr.json")
            << zarr_v3_metadata(shape, chunks, fill_value, dim_names);
    }
    MPI_Barrier(comm);

    for (const Chunk& c :
         chunk_array_mpi(comm, local, mybox, shape, chunks, fill_value, write_empty_chunks))
        write_chunk_file(path, c);
    MPI_Barrier(comm);
}

} // namespace zarr_chunk
