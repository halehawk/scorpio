// ZarrWritePlugin: see ZarrWritePlugin.h.

#include "ZarrWritePlugin.h"

#include <algorithm>
#include <cctype>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>

namespace adios2
{
namespace plugin
{

namespace
{

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Types zarr_chunk can store.
template <typename T>
constexpr bool IsZarrType =
    std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> || std::is_same_v<T, int32_t> ||
    std::is_same_v<T, int64_t> || std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
    std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, float> ||
    std::is_same_v<T, double>;

std::string JSONString(const std::string &s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c == '\n')
            out += "\\n";
        else if (c < 0x20)
        {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        }
        else
            out += char(c);
    }
    return out + "\"";
}

template <typename T>
std::string JSONValue(const T &v)
{
    if constexpr (std::is_same_v<T, std::string>)
        return JSONString(v);
    else
        return zarr_chunk::detail::json_number(v);
}

template <typename T>
T ParseValue(const std::string &s)
{
    if constexpr (std::is_floating_point_v<T>)
        return static_cast<T>(std::strtod(s.c_str(), nullptr));  // also "nan", "inf"
    else if constexpr (std::is_signed_v<T>)
        return static_cast<T>(std::strtoll(s.c_str(), nullptr, 10));
    else
        return static_cast<T>(std::strtoull(s.c_str(), nullptr, 10));
}

void WriteFile(const std::string &path, const std::string &text)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path);
    out << text;
    if (!out)
        throw std::ios_base::failure("ZarrWritePlugin: cannot write " + path);
}

} // namespace

ZarrWritePlugin::ZarrWritePlugin(adios2::IO io, const std::string &name, const Mode mode)
: PluginEngineInterface(io, name, mode)
{
    if (mode != Mode::Write)
        throw std::invalid_argument("ZarrWritePlugin only supports Mode::Write");
#ifdef ZARR_PLUGIN_MPI
    MPI_Comm_dup(MPI_COMM_WORLD, &m_ZComm);
    MPI_Comm_rank(m_ZComm, &m_Rank);
#endif
}

ZarrWritePlugin::~ZarrWritePlugin()
{
#ifdef ZARR_PLUGIN_MPI
    int finalized = 0;
    MPI_Finalized(&finalized);
    if (!finalized && m_ZComm != MPI_COMM_NULL)
        MPI_Comm_free(&m_ZComm);
#endif
}

void ZarrWritePlugin::Barrier()
{
#ifdef ZARR_PLUGIN_MPI
    MPI_Barrier(m_ZComm);
#endif
}

bool ZarrWritePlugin::AnyRank(bool flag)
{
#ifdef ZARR_PLUGIN_MPI
    int in = flag, out = 0;
    MPI_Allreduce(&in, &out, 1, MPI_INT, MPI_MAX, m_ZComm);
    return out != 0;
#else
    return flag;
#endif
}

bool ZarrWritePlugin::SameOnAllRanks(long n)
{
#ifdef ZARR_PLUGIN_MPI
    long nmin = 0, nmax = 0;
    MPI_Allreduce(&n, &nmin, 1, MPI_LONG, MPI_MIN, m_ZComm);
    MPI_Allreduce(&n, &nmax, 1, MPI_LONG, MPI_MAX, m_ZComm);
    return nmin == nmax;
#else
    (void)n;
    return true;
#endif
}

void ZarrWritePlugin::Init()
{
    for (const auto &p : m_IO.Parameters())
        m_Params[Lower(p.first)] = p.second;
    m_WriteEmptyChunks = Lower(Param("WriteEmptyChunks", "false")) == "true";

    // ADIOS2 (2.12) passes the plugin name, not the name given to Open(), as
    // the engine name, so the output path comes from a parameter.
    m_Root = Param("FileName", Param("DirName"));
    if (m_Root.empty())
        throw std::invalid_argument("ZarrWritePlugin: set the FileName parameter to the "
                                    "output directory, e.g. out.zarr");
    if (m_Rank == 0)
        WriteFile(m_Root + "/zarr.json",
                  "{\"zarr_format\": 3, \"node_type\": \"group\", \"attributes\": {}}\n");
    Barrier();
}

std::string ZarrWritePlugin::Param(const std::string &key, const std::string &def) const
{
    auto it = m_Params.find(Lower(key));
    return it == m_Params.end() ? def : it->second;
}

std::vector<size_t> ZarrWritePlugin::ParseList(const std::string &s) const
{
    std::vector<size_t> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        out.push_back(std::stoull(item));
    return out;
}

StepStatus ZarrWritePlugin::BeginStep(StepMode, const float)
{
    m_InStep = true;
    return StepStatus::OK;
}

size_t ZarrWritePlugin::CurrentStep() const { return m_CurrentStep; }

template <typename T>
void ZarrWritePlugin::Put(adios2::Variable<T> variable, const T *values, bool sync)
{
    if constexpr (!IsZarrType<T>)
        return;
    else
    {
        Block b;
        if (variable.ShapeID() == adios2::ShapeID::GlobalValue)
            b.box = {{}, {}};
        else
            b.box = {variable.Start(), variable.Count()};
        if (sync)
        {
            size_t n = zarr_chunk::box_size(b.box) * sizeof(T);
            b.copy.assign(reinterpret_cast<const char *>(values),
                          reinterpret_cast<const char *>(values) + n);
        }
        else
            b.values = values;
        m_Pending[variable.Name()].push_back(std::move(b));
    }
}

#define declare(T)                                                                                 \
    void ZarrWritePlugin::DoPutSync(adios2::Variable<T> variable, const T *values)                 \
    {                                                                                              \
        Put(variable, values, true);                                                               \
    }                                                                                              \
    void ZarrWritePlugin::DoPutDeferred(adios2::Variable<T> variable, const T *values)             \
    {                                                                                              \
        Put(variable, values, false);                                                              \
    }
ADIOS2_FOREACH_STDTYPE_1ARG(declare)
#undef declare

// Deferred Puts may reuse their buffers after PerformPuts: copy them now.
void ZarrWritePlugin::PerformPuts()
{
    for (auto &[name, blocks] : m_Pending)
    {
        const std::string type = m_IO.VariableType(name);
        size_t elem = 0;
#define declare(T)                                                                                 \
    if (type == adios2::GetType<T>())                                                              \
        elem = sizeof(T);
        ADIOS2_FOREACH_STDTYPE_1ARG(declare)
#undef declare
        for (Block &b : blocks)
            if (b.values)
            {
                const char *p = static_cast<const char *>(b.values);
                b.copy.assign(p, p + zarr_chunk::box_size(b.box) * elem);
                b.values = nullptr;
            }
    }
}

void ZarrWritePlugin::EndStep()
{
    Flush();
    m_CurrentStep++;
    m_InStep = false;
    WriteMetadata();
}

void ZarrWritePlugin::DoClose(const int)
{
    // Puts made outside BeginStep/EndStep form one last step.
    if (AnyRank(m_InStep || !m_Pending.empty()))
        EndStep();
    Barrier();
}

// Chunk and write every variable for the current step (collective).
void ZarrWritePlugin::Flush()
{
    std::vector<std::string> names;
    for (const auto &v : m_IO.AvailableVariables(true))
        names.push_back(v.first);  // std::map: same order on every rank

    if (!SameOnAllRanks(static_cast<long>(names.size())))
        throw std::runtime_error("ZarrWritePlugin: ranks define different variables");

    for (const std::string &name : names)
    {
        const std::string type = m_IO.VariableType(name);
#define declare(T)                                                                                 \
    if (type == adios2::GetType<T>())                                                              \
        FlushVariable<T>(name);
        ADIOS2_FOREACH_STDTYPE_1ARG(declare)
#undef declare
    }
    m_Pending.clear();
}

template <typename T>
void ZarrWritePlugin::FlushVariable(const std::string &name)
{
    adios2::Variable<T> var = m_IO.InquireVariable<T>(name);
    const adios2::ShapeID sid = var.ShapeID();
    if constexpr (!IsZarrType<T>)
    {
        if (m_Rank == 0 && m_Skipped.insert(name).second)
            std::cerr << "ZarrWritePlugin: skipping " << name << " (type " << var.Type()
                      << " not supported)\n";
        return;
    }
    else
    {
        if (sid != adios2::ShapeID::GlobalArray && sid != adios2::ShapeID::GlobalValue)
        {
            if (m_Rank == 0 && m_Skipped.insert(name).second)
                std::cerr << "ZarrWritePlugin: skipping " << name
                          << " (only global arrays and global values are written)\n";
            return;
        }

        const std::vector<size_t> shape =
            sid == adios2::ShapeID::GlobalValue ? std::vector<size_t>{} : var.Shape();
        const size_t nd = shape.size();

        std::vector<size_t> chunks = ParseList(Param("ChunkShape." + name));
        if (chunks.empty())
        {
            chunks = ParseList(Param("ChunkShape"));
            if (chunks.size() != nd)
                chunks = shape;
        }
        if (chunks.size() != nd)
            throw std::invalid_argument("ZarrWritePlugin: ChunkShape." + name + " has " +
                                        std::to_string(chunks.size()) + " sizes, " + name +
                                        " has " + std::to_string(nd) + " dimensions");
        for (size_t &c : chunks)
            c = std::max<size_t>(c, 1);
        const T fill = ParseValue<T>(Param("FillValue." + name, Param("FillValue", "0")));

        // Zarr array: (step, shape...) with chunks (1, chunks...).
        std::vector<size_t> zshape{m_CurrentStep + 1}, zchunks{1};
        zshape.insert(zshape.end(), shape.begin(), shape.end());
        zchunks.insert(zchunks.end(), chunks.begin(), chunks.end());

        std::string path = name;
        path.erase(0, path.find_first_not_of('/'));
        auto known = m_Vars.find(name);
        if (known == m_Vars.end())
        {
            std::vector<std::string> dims{"step"};
            std::stringstream ss(Param("DimNames." + name));
            std::string d;
            while (std::getline(ss, d, ','))
                dims.push_back(d);
            const std::string base = path.substr(path.find_last_of('/') + 1);
            if (dims.size() != nd + 1)
            {
                dims.resize(1);
                for (size_t i = 0; i < nd; i++)
                    dims.push_back(base + "_dim" + std::to_string(i));
            }
            m_Vars[name] = ZarrVar{
                path, [=](size_t nsteps, const std::string &attrs) {
                    std::vector<size_t> s = zshape;
                    s[0] = nsteps;
                    return zarr_chunk::zarr_v3_metadata(s, zchunks, fill, dims, attrs);
                }};
        }

        // This rank's blocks, with the step prepended.
        auto pending = m_Pending.find(name);
        std::vector<const T *> data;
        std::vector<zarr_chunk::Box> boxes;
        if (pending != m_Pending.end())
            for (const Block &b : pending->second)
            {
                if (b.box.count.size() != nd)
                    throw std::invalid_argument("ZarrWritePlugin: block of " + name +
                                                " has the wrong number of dimensions");
                zarr_chunk::Box box{{m_CurrentStep}, {1}};
                box.start.insert(box.start.end(), b.box.start.begin(), b.box.start.end());
                box.count.insert(box.count.end(), b.box.count.begin(), b.box.count.end());
                boxes.push_back(box);
                data.push_back(b.values ? static_cast<const T *>(b.values)
                                        : reinterpret_cast<const T *>(b.copy.data()));
            }

#ifdef ZARR_PLUGIN_MPI
        const auto chunks_out = zarr_chunk::chunk_blocks_mpi<T>(m_ZComm, data, boxes, zshape,
                                                                zchunks, fill, m_WriteEmptyChunks);
#else
        const auto chunks_out =
            zarr_chunk::chunk_blocks<T>(data, boxes, zshape, zchunks, fill, m_WriteEmptyChunks);
#endif
        for (const zarr_chunk::Chunk &c : chunks_out)
            zarr_chunk::write_chunk_file(m_Root + "/" + path, c);
    }
}

// ADIOS attributes as a JSON object: those named <prefix><name> (variable
// attributes), or with group=true those not belonging to any variable.
std::string ZarrWritePlugin::AttributesJSON(const std::string &prefix, bool group)
{
    std::string json = "{";
    bool first = true;
    for (const auto &a : m_IO.AvailableAttributes())
    {
        const std::string &full = a.first;
        std::string key;
        if (group)
        {
            bool of_var = false;
            for (const auto &v : m_Vars)
                of_var |= full.compare(0, v.first.size() + 1, v.first + "/") == 0;
            if (of_var)
                continue;
            key = full;
        }
        else if (full.compare(0, prefix.size(), prefix) == 0)
            key = full.substr(prefix.size());
        else
            continue;

        const std::string type = m_IO.AttributeType(full);
        std::string value;
#define declare(T)                                                                                 \
    if (type == adios2::GetType<T>())                                                              \
    {                                                                                              \
        if constexpr (IsZarrType<T> || std::is_same_v<T, std::string>)                             \
        {                                                                                          \
            adios2::Attribute<T> attr = m_IO.InquireAttribute<T>(full);                            \
            std::vector<T> data = attr.Data();                                                     \
            if (attr.IsValue() && data.size() == 1)                                                \
                value = JSONValue(data[0]);                                                        \
            else                                                                                   \
            {                                                                                      \
                value = "[";                                                                       \
                for (size_t i = 0; i < data.size(); i++)                                           \
                    value += (i ? ", " : "") + JSONValue(data[i]);                                 \
                value += "]";                                                                      \
            }                                                                                      \
        }                                                                                          \
    }
        ADIOS2_FOREACH_ATTRIBUTE_STDTYPE_1ARG(declare)
#undef declare
        if (value.empty())
            continue;
        json += std::string(first ? "" : ", ") + JSONString(key) + ": " + value;
        first = false;
    }
    return json + "}";
}

// Rank 0 (re)writes zarr.json of the group, of any intermediate groups and
// of every array, with the current number of steps.
void ZarrWritePlugin::WriteMetadata()
{
    if (m_Rank == 0)
    {
        std::set<std::string> groups;
        for (const auto &[name, zv] : m_Vars)
        {
            WriteFile(m_Root + "/" + zv.path + "/zarr.json",
                      zv.metadata(m_CurrentStep, AttributesJSON(name + "/", false)));
            for (size_t p = zv.path.find('/'); p != std::string::npos;
                 p = zv.path.find('/', p + 1))
                groups.insert(zv.path.substr(0, p));
        }
        for (const std::string &g : groups)
            WriteFile(m_Root + "/" + g + "/zarr.json",
                      "{\"zarr_format\": 3, \"node_type\": \"group\", \"attributes\": {}}\n");
        WriteFile(m_Root + "/zarr.json",
                  "{\"zarr_format\": 3, \"node_type\": \"group\", \"attributes\": " +
                      AttributesJSON("", true) + "}\n");
    }
    Barrier();
}

} // end namespace plugin
} // end namespace adios2

extern "C" {

adios2::plugin::ZarrWritePlugin *EngineCreate(adios2::IO io, const std::string &name,
                                              const adios2::Mode mode)
{
    return new adios2::plugin::ZarrWritePlugin(io, name, mode);
}

void EngineDestroy(adios2::plugin::ZarrWritePlugin *obj) { delete obj; }
}
