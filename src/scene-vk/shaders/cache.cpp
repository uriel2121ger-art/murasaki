#include "shaders/cache.h"

#include "shaders/glslcompiler.h"
#include "util/sha256.h"

#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace murasaki::scene {

namespace {

constexpr char kMagic[4] = {'W', 'S', 'C', '1'};
constexpr uint32_t kMaxItems = 1u << 20;
constexpr uint32_t kFailureTtlSeconds = 24 * 60 * 60;
constexpr uintmax_t kCacheBytesLimit = 256ull << 20; // the cache directory is trimmed to this

class Writer
{
public:
    void u32(uint32_t v) { raw(&v, 4); }
    void str(const std::string &s)
    {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void raw(const void *p, size_t n) { data.append(static_cast<const char *>(p), n); }
    std::string data;
};

class Reader
{
public:
    explicit Reader(const std::string &d) : m_data(d) {}
    uint32_t u32()
    {
        uint32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::string str()
    {
        const uint32_t n = u32();
        if (n > m_data.size() - std::min(m_pos, m_data.size())) {
            m_ok = false;
            return {};
        }
        std::string s = m_data.substr(m_pos, n);
        m_pos += n;
        return s;
    }
    void raw(void *p, size_t n)
    {
        if (n == 0) {
            return; // an empty vector's data() may be null, which memset and memcpy must not be given
        }
        if (!m_ok || n > m_data.size() - m_pos) {
            m_ok = false;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, m_data.data() + m_pos, n);
        m_pos += n;
    }
    bool ok() const { return m_ok; }
    bool atEnd() const { return m_pos == m_data.size(); }

private:
    const std::string &m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

void writeStage(Writer &w, const StageProgram &s)
{
    w.u32(static_cast<uint32_t>(s.spirv.size()));
    w.raw(s.spirv.data(), s.spirv.size() * 4);
    w.u32(s.blockSize);
    w.u32(static_cast<uint32_t>(s.uniforms.size()));
    for (const UniformInfo &u : s.uniforms) {
        w.str(u.name);
        w.str(u.type);
        w.u32(u.arrayCount);
        w.u32(u.offset);
        w.u32(u.size);
        w.str(u.materialKey);
        w.str(u.defaultText);
        w.u32(u.isColor ? 1 : 0);
        w.u32(u.used ? 1 : 0);
        w.u32(static_cast<uint32_t>(u.input));
        w.u32(static_cast<uint32_t>(u.aux));
        w.str(u.lookupKey);
    }
}

bool readStage(Reader &r, StageProgram &s)
{
    const uint32_t words = r.u32();
    if (words > kMaxItems) {
        return false;
    }
    s.spirv.resize(words);
    r.raw(s.spirv.data(), static_cast<size_t>(words) * 4);
    s.blockSize = r.u32();
    const uint32_t count = r.u32();
    if (count > kMaxItems) {
        return false;
    }
    for (uint32_t i = 0; i < count && r.ok(); ++i) {
        UniformInfo u;
        u.name = r.str();
        u.type = r.str();
        u.arrayCount = r.u32();
        u.offset = r.u32();
        u.size = r.u32();
        u.materialKey = r.str();
        u.defaultText = r.str();
        u.isColor = r.u32() != 0;
        u.used = r.u32() != 0;
        const uint32_t input = r.u32();
        u.input = input <= static_cast<uint32_t>(EngineInput::Last) ? static_cast<EngineInput>(input) : EngineInput::None;
        u.aux = static_cast<int>(r.u32() & 0xFFFF);
        u.lookupKey = r.str();
        s.uniforms.push_back(std::move(u));
    }
    return r.ok();
}

bool validSpirv(const std::vector<uint32_t> &words)
{
    return words.size() >= 5 && words[0] == 0x07230203u && words.size() < (4u << 20);
}

bool validStage(const StageProgram &s)
{
    if (!validSpirv(s.spirv) || s.blockSize > kMaxUniformBlock || s.blockSize % 16 != 0) {
        return false;
    }
    for (const UniformInfo &u : s.uniforms) {
        if (u.name.empty() || u.size == 0 || static_cast<uint64_t>(u.offset) + u.size > s.blockSize) {
            return false;
        }
    }
    return s.uniforms.empty() == (s.blockSize == 0);
}

// What a cache file must satisfy before the renderer may trust it: it came from translateShader() once, but the file
// is only a file.
bool validShader(const TranslatedShader &t)
{
    if (!validStage(t.vertex) || !validStage(t.fragment)) {
        return false;
    }
    for (const SamplerInfo &s : t.samplers) {
        if (s.binding > 63 || s.name.empty()) {
            return false;
        }
    }
    for (const AttributeInfo &a : t.attributes) {
        if (a.location > 63) {
            return false;
        }
    }
    return true;
}

// Keeps the cache directory under its size limit by removing the least recently written files; run now and then.
void trimCache(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uintmax_t total = 0;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
            files.emplace_back(entry.last_write_time(ec), entry.path());
        }
    }
    if (total <= kCacheBytesLimit) {
        return;
    }
    std::sort(files.begin(), files.end());
    for (const auto &[time, path] : files) {
        if (total <= kCacheBytesLimit * 3 / 4) {
            break;
        }
        const uintmax_t size = fs::file_size(path, ec);
        if (fs::remove(path, ec)) {
            total -= std::min(total, size);
        }
    }
}

std::string pathFor(const std::string &key)
{
    const std::string dir = shaderCacheDirectory();
    return dir.empty() ? std::string() : dir + "/" + key + ".bin";
}

} // namespace

std::string shaderCacheKey(const ShaderSource &vertex, const ShaderSource &fragment, const std::map<std::string, int> &comboOverrides,
                           const std::vector<uint32_t> &boundTextures)
{
    std::string material = "translator " + std::to_string(kTranslatorVersion) + "\n";
    for (const auto &[name, value] : comboOverrides) {
        std::string up = name;
        for (char &c : up) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        material += up + "=" + std::to_string(value) + "\n";
    }
    std::vector<uint32_t> bound = boundTextures;
    std::sort(bound.begin(), bound.end());
    for (uint32_t b : bound) {
        material += "tex" + std::to_string(b) + "\n";
    }
    // SHA-256 over everything that decides the result, so two different inputs never share a file.
    Sha256 h;
    h.update(material);
    h.update("\ncompiler " + compilerVersion() + "\nvertex\n");
    h.update(vertex.text);
    h.update("\nfragment\n");
    h.update(fragment.text);
    return h.hex();
}

CachedShader loadCachedShader(const std::string &key, std::string *error)
{
    CachedShader out;
    const std::string path = pathFor(key);
    if (path.empty()) {
        return out;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return out;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Reader r(data);
    char magic[4];
    r.raw(magic, 4);
    const uint32_t status = r.u32();
    if (!r.ok() || std::memcmp(magic, kMagic, 4) != 0 || status > 1) {
        return out; // unreadable: treated as absent and overwritten
    }
    if (status == 0) {
        const uint32_t storedAt = r.u32(); // seconds since the epoch, low 32 bits
        const std::string message = r.str();
        if (!r.ok() || message.size() > (64u << 10)) {
            return out;
        }
        // a failure is believed for a day: the compiler, the assets or the engine may have changed since
        const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        if (now - storedAt > kFailureTtlSeconds && now >= storedAt) {
            return out;
        }
        out.hit = true;
        if (error) {
            *error = message;
        }
        return out;
    }
    TranslatedShader shader;
    if (!readStage(r, shader.vertex) || !readStage(r, shader.fragment)) {
        return out;
    }
    const uint32_t samplers = r.u32();
    if (samplers > kMaxItems) {
        return out;
    }
    for (uint32_t i = 0; i < samplers && r.ok(); ++i) {
        SamplerInfo s;
        s.name = r.str();
        s.binding = r.u32();
        s.type = r.str();
        s.comboName = r.str();
        s.defaultText = r.str();
        s.hidden = r.u32() != 0;
        shader.samplers.push_back(std::move(s));
    }
    const uint32_t attributes = r.u32();
    if (attributes > kMaxItems) {
        return out;
    }
    for (uint32_t i = 0; i < attributes && r.ok(); ++i) {
        AttributeInfo a;
        a.name = r.str();
        a.type = r.str();
        a.location = r.u32();
        shader.attributes.push_back(std::move(a));
    }
    const uint32_t combos = r.u32();
    if (combos > kMaxItems) {
        return out;
    }
    for (uint32_t i = 0; i < combos && r.ok(); ++i) {
        std::string name = r.str();
        shader.combos[name] = static_cast<int>(r.u32());
    }
    if (!r.ok() || !r.atEnd() || !validShader(shader)) {
        return out; // damaged or tampered with: treated as absent and overwritten
    }
    out.hit = true;
    out.shader = std::move(shader);
    return out;
}

void storeCachedShader(const std::string &key, const TranslatedShader *shader, const std::string &failure)
{
    const std::string path = pathFor(key);
    if (path.empty()) {
        return;
    }
    Writer w;
    w.raw(kMagic, 4);
    if (!shader) {
        w.u32(0);
        w.u32(static_cast<uint32_t>(std::time(nullptr)));
        w.str(failure.substr(0, 64u << 10));
    } else {
        w.u32(1);
        writeStage(w, shader->vertex);
        writeStage(w, shader->fragment);
        w.u32(static_cast<uint32_t>(shader->samplers.size()));
        for (const SamplerInfo &s : shader->samplers) {
            w.str(s.name);
            w.u32(s.binding);
            w.str(s.type);
            w.str(s.comboName);
            w.str(s.defaultText);
            w.u32(s.hidden ? 1 : 0);
        }
        w.u32(static_cast<uint32_t>(shader->attributes.size()));
        for (const AttributeInfo &a : shader->attributes) {
            w.str(a.name);
            w.str(a.type);
            w.u32(a.location);
        }
        w.u32(static_cast<uint32_t>(shader->combos.size()));
        for (const auto &[name, value] : shader->combos) {
            w.str(name);
            w.u32(static_cast<uint32_t>(value));
        }
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string temp = path + "." + std::to_string(::getpid()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(w.data.data(), static_cast<std::streamsize>(w.data.size()));
        if (!out) {
            fs::remove(temp, ec);
            return;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
    }
    static unsigned stores = 0;
    if (++stores % 64 == 1) {
        trimCache(fs::path(path).parent_path().string());
    }
}

} // namespace murasaki::scene
