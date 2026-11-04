// ZarrWritePlugin: an ADIOS2 engine plugin that writes the variables of an
// IO as a Zarr v3 group, chunked in parallel with zarr_chunk_mpi.hpp.
//
// The Zarr group directory (e.g. "out.zarr") is set with the FileName
// parameter: ADIOS2 2.12 hands plugins the plugin name, not the name given
// to Open().
// Each global array or global value <var> becomes the Zarr array <var> with
// a leading "step" dimension: ADIOS step s is index s of that dimension.
// ADIOS attributes "<var>/<name>" become attributes of <var>; the others
// become attributes of the group.
//
// Engine parameters (names are case insensitive):
//   PluginName        ZarrWrite
//   PluginLibrary     adios2_zarr_plugin
//   FileName          output directory (required; DirName also accepted)
//   ChunkShape        default chunk shape "c0,c1,..." for arrays of that rank
//   ChunkShape.<var>  chunk shape of <var> (not counting the step dimension);
//                     default: the whole array, i.e. one chunk per step
//   FillValue         default fill value (default 0)
//   FillValue.<var>   fill value of <var>
//   DimNames.<var>    dimension names "y,x" of <var> (default <var>_dim<i>)
//   WriteEmptyChunks  true/false (default false, as zarr-python)
//
// Serial build (default): no MPI. Chunks are assembled from the blocks this
// process Put, so the output is complete for a single writer process. With
// several writer processes it is correct only if every chunk lies entirely
// within the blocks of one process (and the processes must not share an
// output directory otherwise, since they would overwrite each other).
//
// MPI build (-DZARR_PLUGIN_MPI, CMake option ZARR_PLUGIN_USE_MPI): chunks are
// assembled across ranks. ADIOS2 2.12 does not hand plugins the communicator
// given to adios2::ADIOS, so the plugin uses (a duplicate of)
// MPI_COMM_WORLD; every rank of MPI_COMM_WORLD must open the engine.
//
// Restrictions: every rank must define the same variables; local arrays,
// local values and joined arrays, and string/char/long double/complex
// variables, are skipped. Chunks are not compressed.

#ifndef ZARRWRITEPLUGIN_H_
#define ZARRWRITEPLUGIN_H_

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "adios2/plugin/PluginEngineInterface.h"

// Built with ZARR_PLUGIN_MPI the plugin assembles chunks across ranks;
// otherwise it is serial (no MPI at all) and each process can only build
// chunks from the blocks it Put itself.
#ifdef ZARR_PLUGIN_MPI
#if !ADIOS2_USE_MPI
#error "ZARR_PLUGIN_MPI needs ADIOS2's MPI interface (link adios2::cxx_mpi)"
#endif
#include "../zarr_chunk_mpi.hpp"
#else
#include "../zarr_chunk.hpp"
#endif

namespace adios2
{
namespace plugin
{

class ZarrWritePlugin : public PluginEngineInterface
{
public:
    ZarrWritePlugin(adios2::IO io, const std::string &name, const Mode mode);
    ~ZarrWritePlugin() override;

    StepStatus BeginStep(StepMode mode, const float timeoutSeconds = -1.0f) override;
    void EndStep() override;
    size_t CurrentStep() const override;
    void PerformPuts() override;

protected:
    void Init() override;

#define declare(T)                                                                                 \
    void DoPutSync(adios2::Variable<T> variable, const T *values) override;                        \
    void DoPutDeferred(adios2::Variable<T> variable, const T *values) override;
    ADIOS2_FOREACH_STDTYPE_1ARG(declare)
#undef declare

    void DoClose(const int transportIndex = -1) override;

private:
    // One block Put by this rank in the current step.
    struct Block
    {
        zarr_chunk::Box box;           // without the step dimension
        const void *values = nullptr;  // deferred Put: the caller's buffer
        std::vector<char> copy;        // sync Put, or after PerformPuts
    };

    // A variable written as a Zarr array.
    struct ZarrVar
    {
        std::string path;  // relative to the group directory
        // zarr.json for a given number of steps and attributes JSON
        std::function<std::string(size_t, const std::string &)> metadata;
    };

#ifdef ZARR_PLUGIN_MPI
    // ADIOS2 (2.12) calls EngineCreate without a communicator even in MPI
    // builds, so the plugin works on a duplicate of MPI_COMM_WORLD.
    MPI_Comm m_ZComm = MPI_COMM_NULL;
#endif
    int m_Rank = 0;
    std::string m_Root;
    size_t m_CurrentStep = 0;
    bool m_InStep = false;
    bool m_WriteEmptyChunks = false;
    std::map<std::string, std::string> m_Params;  // lower-cased keys
    std::map<std::string, std::vector<Block>> m_Pending;
    std::map<std::string, ZarrVar> m_Vars;
    std::set<std::string> m_Skipped;  // variables already warned about

    std::string Param(const std::string &key, const std::string &def = "") const;
    std::vector<size_t> ParseList(const std::string &s) const;

    template <typename T>
    void Put(adios2::Variable<T> variable, const T *values, bool sync);

    void Flush();
    template <typename T>
    void FlushVariable(const std::string &name);
    void WriteMetadata();

    // Collective helpers; no-ops (single process) in the serial build.
    void Barrier();
    bool AnyRank(bool flag);
    bool SameOnAllRanks(long n);
    std::string AttributesJSON(const std::string &prefix, bool group);
};

} // end namespace plugin
} // end namespace adios2

extern "C" {

adios2::plugin::ZarrWritePlugin *EngineCreate(adios2::IO io, const std::string &name,
                                              const adios2::Mode mode);
void EngineDestroy(adios2::plugin::ZarrWritePlugin *obj);
}

#endif /* ZARRWRITEPLUGIN_H_ */
