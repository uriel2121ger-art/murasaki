#include "shaders/glslcompiler.h"

#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>
#else
#include <SPIRV/GlslangToSpv.h>
#endif
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "util/sha256.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <functional>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <atomic>

namespace murasaki::scene {

namespace {

struct Outcome {
    bool ok = false;
    std::string data;
    std::string error;
};

void configure(glslang::TShader &shader, EShLanguage language)
{
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
}

constexpr EShMessages kMessages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

void ensureGlslangInitialised()
{
    static std::once_flag once;
    std::call_once(once, [] {
        glslang::InitializeProcess();
        // Finalised by the OS at exit; the compiler is needed until then.
        // glslang builds the symbol tables of its built-ins on the first parse of each stage and keeps them for the process.
        // Built here, in the parent, from a fixed shader of ours, every forked compiler inherits them: built in the child they
        // were thrown away with it, and cost about a second of CPU per shader.
        for (const EShLanguage language : {EShLangVertex, EShLangFragment}) {
            glslang::TShader shader(language);
            const char *strings[] = {"#version 450\nvoid main() {}\n"};
            shader.setStrings(strings, 1);
            configure(shader, language);
            shader.parse(GetDefaultResources(), 450, false, kMessages);
        }
    });
}

// What the compiler made of a source in this process, success or failure, by a hash of the stage, the operation and the text:
// a scene names the same shader from many layers and a failing one is retried by the shim, and none of it needs a second
// compiler run. Bounded: past kMemoBytes it starts again.
constexpr size_t kMemoBytes = 64u << 20;

struct Memo {
    std::mutex mutex;
    std::map<std::string, Outcome> entries;
    size_t bytes = 0;
};

Memo &memo()
{
    static Memo m;
    return m;
}

std::string memoKey(const char *operation, ShaderStage stage, std::string_view source)
{
    Sha256 h;
    h.update(operation);
    h.update(stage == ShaderStage::Vertex ? "vert" : "frag");
    h.update(source);
    return h.hex();
}

std::optional<Outcome> remembered(const std::string &key)
{
    Memo &m = memo();
    std::lock_guard lock(m.mutex);
    if (const auto it = m.entries.find(key); it != m.entries.end()) {
        return it->second;
    }
    return std::nullopt;
}

void remember(const std::string &key, const Outcome &outcome)
{
    Memo &m = memo();
    std::lock_guard lock(m.mutex);
    const size_t size = key.size() + outcome.data.size() + outcome.error.size();
    if (m.bytes + size > kMemoBytes) {
        m.entries.clear();
        m.bytes = 0;
    }
    if (m.entries.emplace(key, outcome).second) {
        m.bytes += size;
    }
}

std::atomic<uint64_t> g_compilerRuns{0};

// Bump when the compiler options below change, so old cache files are not picked up. The glslang version is part
// of the key too.
constexpr std::string_view kCompilerSetup = "glslang vulkan1.3 spirv1.6 glsl450 v2";

std::string cachePathFor(ShaderStage stage, std::string_view source)
{
    const std::string dir = shaderCacheDirectory();
    if (dir.empty()) {
        return {};
    }
    Sha256 h;
    h.update(kCompilerSetup);
    h.update(compilerVersion());
    h.update(stage == ShaderStage::Vertex ? "vert" : "frag");
    h.update(source);
    return dir + "/" + h.hex() + ".spv";
}

// ---- running glslang in a child process -------------------------------------------------------------------------
//
// Shader text is untrusted: a handful of chained #defines expands exponentially and a preprocessor can then run for
// minutes and eat all memory. Preprocessing and compiling therefore run in a forked child with a CPU limit, an
// address space limit and a wall-clock timeout, and only the result crosses a pipe. The parent initialises glslang
// first so the child inherits its tables instead of building them again.

constexpr rlim_t kChildCpuSeconds = 5;
constexpr rlim_t kChildAddressSpace = 2ull << 30; // on top of what the process has mapped when it forks

// The process's mapped address space (the first field of /proc/self/statm, in pages); 0 when it cannot be read. Read by the parent
// before it forks: under the sandbox a child's /proc/self is another process's directory, which Landlock does not open.
rlim_t mappedBytes()
{
    const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    char buffer[64] = {};
    const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    rlim_t pages = 0;
    for (ssize_t i = 0; i < n && buffer[i] >= '0' && buffer[i] <= '9'; ++i) {
        pages = pages * 10 + static_cast<rlim_t>(buffer[i] - '0');
    }
    return pages * static_cast<rlim_t>(::sysconf(_SC_PAGESIZE));
}
constexpr std::chrono::seconds kChildWallTimeout{15};
constexpr size_t kMaxChildOutput = 8u << 20;
constexpr size_t kMaxPreprocessed = 4u << 20; // a real shader preprocesses to well under 1 MiB


void writeAll(int fd, const void *p, size_t n)
{
    const char *c = static_cast<const char *>(p);
    while (n > 0) {
        const ssize_t w = ::write(fd, c, n);
        if (w <= 0) {
            return;
        }
        c += w;
        n -= static_cast<size_t>(w);
    }
}

Outcome runLimited(const std::function<Outcome()> &work)
{
#ifdef __SANITIZE_ADDRESS__
    // The asan preset: the sanitizer maps terabytes of shadow memory (an address space limit would kill the child) and a
    // forked, sanitized child does not finish. The compiler runs in this process there; the limits are for real builds.
    try {
        return work();
    } catch (const std::exception &e) {
        return {false, {}, std::string("shader compiler failed: ") + e.what()};
    }
#endif
    int fds[2];
    if (::pipe(fds) != 0) {
        return {false, {}, "cannot create a pipe for the shader compiler"};
    }
    const rlim_t parentMapped = mappedBytes();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return {false, {}, "cannot start the shader compiler process"};
    }
    if (pid == 0) {
        ::close(fds[0]);
        // hitting a limit is an expected outcome, not a crash: no core dump, so the desktop's crash reporter stays quiet
        rlimit core{0, 0};
        ::setrlimit(RLIMIT_CORE, &core);
        ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        rlimit cpu{kChildCpuSeconds, kChildCpuSeconds + 1};
        ::setrlimit(RLIMIT_CPU, &cpu);
        // the limit is on what the compiler adds: the child starts as a copy of the parent, whose mappings (a Vulkan driver, a video
        // decoder) can already exceed the allowance by themselves, and a limit under the current size fails every allocation. When
        // the size is not known there is no such limit; the CPU limit, the clock and the output caps still bound the child.
        if (parentMapped > 0) {
            rlimit as{parentMapped + kChildAddressSpace, parentMapped + kChildAddressSpace};
            ::setrlimit(RLIMIT_AS, &as);
        }
        Outcome out;
        try {
            out = work();
        } catch (const std::exception &e) {
            out = {false, {}, std::string("shader compiler failed: ") + e.what()};
        }
        const uint8_t ok = out.ok ? 1 : 0;
        const uint64_t dataSize = out.data.size();
        const uint64_t errorSize = out.error.size();
        writeAll(fds[1], &ok, 1);
        writeAll(fds[1], &dataSize, 8);
        writeAll(fds[1], &errorSize, 8);
        writeAll(fds[1], out.data.data(), out.data.size());
        writeAll(fds[1], out.error.data(), out.error.size());
        ::_exit(0);
    }
    ::close(fds[1]);
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + kChildWallTimeout;
    bool timedOut = false;
    char buffer[65536];
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int r = ::poll(&p, 1, static_cast<int>(std::min<long>(left, 1000)));
        if (r < 0 && errno != EINTR) {
            break;
        }
        if (r > 0) {
            const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            received.append(buffer, static_cast<size_t>(n));
            if (received.size() > kMaxChildOutput + 32) {
                timedOut = true; // more output than any shader can produce
                break;
            }
        }
    }
    ::close(fds[0]);
    if (timedOut) {
        ::kill(pid, SIGKILL);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (timedOut) {
        return {false, {}, "the shader compiler exceeded its limits (time or output size)"};
    }
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        return {false, {}, sig == SIGXCPU || sig == SIGKILL ? std::string("the shader compiler exceeded its limits (CPU time or memory)")
                                                          : "the shader compiler crashed (signal " + std::to_string(sig) + ")"};
    }
    if (received.size() < 17) {
        return {false, {}, "the shader compiler ran out of memory or stopped without an answer"};
    }
    uint64_t dataSize = 0;
    uint64_t errorSize = 0;
    std::memcpy(&dataSize, received.data() + 1, 8);
    std::memcpy(&errorSize, received.data() + 9, 8);
    if (dataSize > kMaxChildOutput || errorSize > kMaxChildOutput || 17 + dataSize + errorSize != received.size()) {
        return {false, {}, "the shader compiler gave a malformed answer"};
    }
    Outcome out;
    out.ok = received[0] == 1;
    out.data = received.substr(17, dataSize);
    out.error = received.substr(17 + dataSize, errorSize);
    return out;
}

std::vector<uint32_t> readCache(const std::string &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const std::streamsize size = in.tellg();
    if (size < 20 || size % 4 != 0 || size > (16 << 20)) {
        return {};
    }
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char *>(words.data()), size);
    return in && words[0] == 0x07230203u ? words : std::vector<uint32_t>{};
}

void writeCache(const std::string &path, const std::vector<uint32_t> &words)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    // Write beside the final name and rename, so a reader never sees half a file.
    const std::string temp = path + "." + std::to_string(::getpid()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(words.data()), static_cast<std::streamsize>(words.size() * 4));
        if (!out) {
            fs::remove(temp, ec);
            return;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
    }
}

} // namespace

std::string shaderCacheDirectory()
{
    if (const char *off = std::getenv("MURASAKI_SHADER_CACHE"); off && std::strcmp(off, "0") == 0) {
        return {};
    }
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg == '/') {
        return std::string(xdg) + "/murasaki/shader-cache";
    }
    if (const char *home = std::getenv("HOME"); home && *home == '/') {
        return std::string(home) + "/.cache/murasaki/shader-cache";
    }
    return {};
}

std::string compilerVersion()
{
    const auto v = glslang::GetVersion();
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

std::string preprocessGlsl(ShaderStage stage, std::string_view source, std::string &error)
{
    ensureGlslangInitialised();
    const std::string key = memoKey("preprocess", stage, source);
    std::optional<Outcome> outcome = remembered(key);
    if (!outcome) {
        const std::string text(source);
        ++g_compilerRuns;
        outcome = runLimited([&]() -> Outcome {
            const EShLanguage language = stage == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
            glslang::TShader shader(language);
            const char *strings[] = {text.c_str()};
            shader.setStrings(strings, 1);
            configure(shader, language);
            std::string out;
            glslang::TShader::ForbidIncluder includer;
            if (!shader.preprocess(GetDefaultResources(), 450, ENoProfile, false, false, kMessages, &out, includer)) {
                return {false, {}, std::string(shader.getInfoLog()) + shader.getInfoDebugLog()};
            }
            if (out.size() > kMaxPreprocessed) {
                return {false, {}, "the preprocessed shader is larger than " + std::to_string(kMaxPreprocessed >> 10) + " KiB (macro expansion)"};
            }
            return {true, std::move(out), {}};
        });
        remember(key, *outcome);
    }
    if (!outcome->ok) {
        error = outcome->error;
        return {};
    }
    return outcome->data;
}

std::vector<uint32_t> compileGlsl(ShaderStage stage, std::string_view source, std::string &error)
{
    const std::string key = memoKey("compile", stage, source);
    std::optional<Outcome> outcome = remembered(key);
    const std::string cachePath = outcome ? std::string() : cachePathFor(stage, source);
    if (!cachePath.empty()) {
        std::vector<uint32_t> cached = readCache(cachePath);
        if (!cached.empty()) {
            return cached;
        }
    }
    if (!outcome) {
        ensureGlslangInitialised();
        const std::string text(source);
        ++g_compilerRuns;
        outcome = runLimited([&]() -> Outcome {
            const EShLanguage language = stage == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
            glslang::TShader shader(language);
            const char *strings[] = {text.c_str()};
            shader.setStrings(strings, 1);
            configure(shader, language);
            if (!shader.parse(GetDefaultResources(), 450, false, kMessages)) {
                return {false, {}, std::string(shader.getInfoLog()) + shader.getInfoDebugLog()};
            }
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(kMessages)) {
                return {false, {}, std::string(program.getInfoLog()) + program.getInfoDebugLog()};
            }
            std::vector<uint32_t> spirv;
            glslang::GlslangToSpv(*program.getIntermediate(language), spirv);
            if (spirv.empty()) {
                return {false, {}, "glslang produced no SPIR-V"};
            }
            return {true, std::string(reinterpret_cast<const char *>(spirv.data()), spirv.size() * 4), {}};
        });
        // a resource limit is about this run (a loaded machine), not about the source: it is not remembered
        if (outcome->ok || outcome->error.find("exceeded its limits") == std::string::npos) {
            remember(key, *outcome);
        }
        if (outcome->ok && !cachePath.empty()) {
            std::vector<uint32_t> spirv(outcome->data.size() / 4);
            std::memcpy(spirv.data(), outcome->data.data(), spirv.size() * 4);
            writeCache(cachePath, spirv);
            return spirv;
        }
    }
    if (!outcome->ok) {
        error = outcome->error;
        return {};
    }
    std::vector<uint32_t> spirv(outcome->data.size() / 4);
    std::memcpy(spirv.data(), outcome->data.data(), spirv.size() * 4);
    return spirv;
}

uint64_t glslCompilerRuns()
{
    return g_compilerRuns.load();
}

} // namespace murasaki::scene
