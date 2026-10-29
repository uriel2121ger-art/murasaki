#include "profile.h"

#include <elf.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace murasaki::sandbox {

namespace {

std::string cacheHome(const ProfileEnvironment &env)
{
    return !env.xdgCacheHome.empty() ? env.xdgCacheHome : env.home + "/.cache";
}

std::string configHome(const ProfileEnvironment &env)
{
    return !env.xdgConfigHome.empty() ? env.xdgConfigHome : env.home + "/.config";
}

// Both caches live under one directory of ours: the engines' own (translated shaders) and the GPU drivers'.
std::string murasakiCache(const ProfileEnvironment &env)
{
    return cacheHome(env) + "/murasaki";
}

void appendGlob(std::vector<std::string> &into, const char *pattern)
{
    glob_t found{};
    if (::glob(pattern, 0, nullptr, &found) == 0) {
        for (size_t i = 0; i < found.gl_pathc; ++i)
            into.emplace_back(found.gl_pathv[i]);
    }
    ::globfree(&found);
}

// The directories a 64 bit ELF executable lists as DT_RUNPATH / DT_RPATH ($ORIGIN expanded): where the loader will look
// for libraries beyond the system's, so the sandbox has to let it read them.
std::vector<std::string> elfLibraryPaths(const std::string &file)
{
    std::vector<std::string> result;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return result;
    // Only the pieces that are needed are read: an engine binary with debug information is many megabytes.
    const auto readAt = [&in](uint64_t offset, void *out, size_t size) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(static_cast<char *>(out), static_cast<std::streamsize>(size));
        return in.good() || (in.eof() && in.gcount() == static_cast<std::streamsize>(size));
    };
    Elf64_Ehdr eh;
    if (!readAt(0, &eh, sizeof(eh)) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64
        || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 64)
        return result;
    std::vector<Elf64_Phdr> headers(eh.e_phnum);
    if (!readAt(eh.e_phoff, headers.data(), headers.size() * sizeof(Elf64_Phdr)))
        return result;
    const auto toOffset = [&](uint64_t vaddr, uint64_t &offset) {
        for (const Elf64_Phdr &ph : headers) {
            if (ph.p_type == PT_LOAD && vaddr >= ph.p_vaddr && vaddr < ph.p_vaddr + ph.p_filesz) {
                offset = ph.p_offset + (vaddr - ph.p_vaddr);
                return true;
            }
        }
        return false;
    };
    for (const Elf64_Phdr &ph : headers) {
        if (ph.p_type != PT_DYNAMIC || ph.p_filesz == 0 || ph.p_filesz > (1u << 20))
            continue;
        std::vector<char> dynamic(ph.p_filesz);
        if (!readAt(ph.p_offset, dynamic.data(), dynamic.size()))
            continue;
        std::vector<uint64_t> names; // offsets into the string table
        uint64_t strtab = 0;
        uint64_t strsz = 0;
        for (size_t at = 0; at + sizeof(Elf64_Dyn) <= dynamic.size(); at += sizeof(Elf64_Dyn)) {
            Elf64_Dyn dyn;
            std::memcpy(&dyn, dynamic.data() + at, sizeof(dyn));
            if (dyn.d_tag == DT_NULL)
                break;
            if (dyn.d_tag == DT_STRTAB)
                strtab = dyn.d_un.d_ptr;
            else if (dyn.d_tag == DT_STRSZ)
                strsz = dyn.d_un.d_val;
            else if (dyn.d_tag == DT_RUNPATH || dyn.d_tag == DT_RPATH)
                names.push_back(dyn.d_un.d_val);
        }
        uint64_t base = 0;
        if (names.empty() || strsz == 0 || strsz > (1u << 20) || !toOffset(strtab, base))
            continue;
        std::vector<char> strings(strsz + 1, '\0');
        if (!readAt(base, strings.data(), strsz))
            continue;
        const size_t slash = file.rfind('/');
        const std::string origin = slash == std::string::npos ? "." : file.substr(0, slash);
        for (const uint64_t name : names) {
            if (name >= strsz)
                continue;
            const std::string list(strings.data() + name); // NUL terminated: the last byte is ours
            size_t pos = 0;
            while (pos <= list.size()) {
                const size_t next = list.find(':', pos);
                std::string dir = list.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                for (const char *token : {"$ORIGIN", "${ORIGIN}"}) {
                    for (size_t at = dir.find(token); at != std::string::npos; at = dir.find(token, at))
                        dir.replace(at, std::strlen(token), origin);
                }
                if (!dir.empty() && dir[0] == '/' && dir.find('$') == std::string::npos)
                    result.push_back(dir);
                if (next == std::string::npos)
                    break;
                pos = next + 1;
            }
        }
    }
    return result;
}

// The engine itself (and the libraries and data installed next to it, unless it sits in a generic bin directory)
// plus the place a launcher script points at.
// `engine` false: only the program and the libraries its own RUNPATH names (the screen matching helper), none of the scene engine's extras (its directory, its video plugin, ~/.local/lib/murasaki).
void appendProgramPaths(std::vector<std::string> &into, const std::string &program, const ProfileEnvironment &env, bool engine = true)
{
    std::string found;
    if (program.find('/') != std::string::npos) {
        found = program;
    } else if (const char *path = std::getenv("PATH")) {
        const std::string dirs = path;
        size_t pos = 0;
        while (pos <= dirs.size()) {
            const size_t next = dirs.find(':', pos);
            const std::string dir = dirs.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            const std::string candidate = (dir.empty() ? "." : dir) + "/" + program;
            if (::access(candidate.c_str(), X_OK) == 0) {
                found = candidate;
                break;
            }
            if (next == std::string::npos)
                break;
            pos = next + 1;
        }
    }
    char real[PATH_MAX];
    if (!found.empty() && ::realpath(found.c_str(), real)) {
        into.emplace_back(real);
        const std::string resolved = real;
        for (std::string &dir : elfLibraryPaths(resolved))
            into.push_back(std::move(dir));
        if (!engine)
            return;
        const size_t slash = resolved.rfind('/');
        const std::string dir = slash == std::string::npos || slash == 0 ? std::string() : resolved.substr(0, slash);
        const size_t last = dir.rfind('/');
        const std::string name = last == std::string::npos ? dir : dir.substr(last + 1);
        if (!dir.empty() && name != "bin" && name != "sbin")
            into.push_back(dir);
        // murasaki-scene-vk loads its video decoder (libav) as a plugin when a scene has a video: wherever the engine looks for it (next
        // to itself, in ../lib/murasaki of its prefix: ~/.local, /usr, Flatpak's /app), the plugin, the directories it lists and the
        // directory it sits in (and the lib directory above that, where its libav libraries may be) are readable
        if (!dir.empty()) {
            for (const char *relative : {"", "/../lib/murasaki", "/../lib64/murasaki", "/../lib/x86_64-linux-gnu/murasaki", "/../lib/aarch64-linux-gnu/murasaki"}) {
                char plugin[PATH_MAX];
                const std::string candidate = dir + relative + "/libmurasaki-scene-vk-video.so";
                if (!::realpath(candidate.c_str(), plugin)) {
                    continue;
                }
                const std::string path = plugin;
                into.push_back(path);
                for (std::string &libDir : elfLibraryPaths(path))
                    into.push_back(std::move(libDir));
                const size_t cut = path.rfind('/');
                if (cut != std::string::npos && cut > 0) {
                    const std::string pluginDir = path.substr(0, cut);
                    into.push_back(pluginDir);
                    const size_t up = pluginDir.rfind('/');
                    if (up != std::string::npos && up > 0 && pluginDir.compare(up + 1, 3, "lib") == 0) {
                        into.push_back(pluginDir.substr(0, up)); // (the lib directory itself: /app/lib)
                    } else if (up != std::string::npos && up > 0) {
                        const std::string parent = pluginDir.substr(0, up);
                        const size_t upper = parent.rfind('/');
                        if (upper != std::string::npos && parent.compare(upper + 1, 3, "lib") == 0) {
                            into.push_back(parent); // /usr/lib or /app/lib above lib/murasaki
                        }
                    }
                }
            }
        }
    }
    static_cast<void>(env);
}

bool mkdirs(const std::string &path)
{
    std::string partial;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t next = path.find('/', pos);
        partial = path.substr(0, next == std::string::npos ? path.size() : next);
        if (!partial.empty() && ::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return true;
}

} // namespace

Policy sceneProfile(const SceneProfileInput &input, const ProfileEnvironment &env)
{
    Policy policy;
    policy.denyTcp = true;
    auto &ro = policy.readOnly;
    auto &rw = policy.readWrite;

    // The item and the Wallpaper Engine assets: everything a scene is made of.
    if (!input.itemDir.empty())
        ro.push_back(input.itemDir);
    if (!input.assetsDir.empty())
        ro.push_back(input.assetsDir);

    if (!input.program.empty())
        appendProgramPaths(ro, input.program, env);

    // The system: programs, libraries (the loader's own configuration included), fonts.
    for (const char *path : {"/usr", "/lib", "/lib32", "/lib64", "/libx32", "/bin", "/sbin", "/etc/ld.so.cache", "/etc/ld.so.conf",
                             "/etc/ld.so.conf.d", "/etc/fonts", "/etc/localtime", "/etc/timezone", "/etc/nsswitch.conf", "/etc/passwd", "/etc/group"})
        ro.emplace_back(path);

    // Fontconfig: the system cache, and the user's configuration, fonts and cache (text scenes pick fonts by name).
    for (const char *path : {"/var/cache/fontconfig", "/usr/local/share/fonts"})
        ro.emplace_back(path);
    ro.push_back(configHome(env) + "/fontconfig");
    ro.push_back(cacheHome(env) + "/fontconfig");
    ro.push_back(env.home + "/.fonts");
    ro.push_back(env.home + "/.local/share/fonts");

    // GPU discovery: the Vulkan loader's ICD and layer manifests, Mesa and glvnd configuration, and the sysfs
    // description of the devices (libdrm reads /sys/dev/char/<major:minor>, which points into /sys/devices).
    for (const char *path : {"/etc/vulkan", "/etc/glvnd", "/etc/drirc", "/sys/devices", "/sys/dev", "/sys/class/drm", "/sys/bus/pci"})
        ro.emplace_back(path);

    // Audio clients read their configuration; the sockets they connect to are not filesystem rights.
    for (const char *path : {"/etc/pipewire", "/etc/pulse", "/etc/alsa", "/etc/asound.conf"})
        ro.emplace_back(path);
    ro.push_back(configHome(env) + "/pipewire");
    ro.push_back(configHome(env) + "/pulse");
    // libpulse opens its runtime directory (it checks who owns it) before it connects to the socket inside.
    if (!env.xdgRuntimeDir.empty())
        ro.push_back(env.xdgRuntimeDir + "/pulse");

    // Information about the process and the machine.
    for (const char *path : {"/proc/self", "/proc/meminfo", "/proc/cpuinfo", "/proc/version", "/proc/sys/vm", "/proc/sys/kernel"})
        ro.emplace_back(path);

    // Threads name themselves by writing /proc/self/task/<tid>/comm (SDL, PipeWire, Chromium do); it is the process's own.
    rw.emplace_back("/proc/self/task");

    // The GPU, and the few device nodes every program uses.
    rw.emplace_back("/dev/dri");
    appendGlob(rw, "/dev/nvidia*");
    for (const char *path : {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random"})
        rw.emplace_back(path);

    // The shader caches.
    rw.push_back(murasakiCache(env) + "/shader-cache");
    rw.push_back(murasakiCache(env) + "/gpu-cache");
    return policy;
}

Policy puppetProfile(const PuppetProfileInput &input, const ProfileEnvironment &env)
{
    Policy policy;
    policy.denyTcp = true;
    auto &ro = policy.readOnly;
    auto &rw = policy.readWrite;
    if (!input.itemDir.empty())
        ro.push_back(input.itemDir);
    if (!input.corePath.empty())
        ro.push_back(input.corePath);
    if (!input.program.empty())
        appendProgramPaths(ro, input.program, env);
    for (const char *path : {"/usr", "/lib", "/lib32", "/lib64", "/libx32", "/bin", "/sbin", "/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/localtime", "/etc/nsswitch.conf"})
        ro.emplace_back(path);
    for (const char *path : {"/etc/vulkan", "/etc/glvnd", "/etc/drirc", "/sys/devices", "/sys/dev", "/sys/class/drm", "/sys/bus/pci"})
        ro.emplace_back(path);
    for (const char *path : {"/proc/self", "/proc/meminfo", "/proc/cpuinfo", "/proc/version", "/proc/sys/vm", "/proc/sys/kernel"})
        ro.emplace_back(path);
    rw.emplace_back("/proc/self/task");
    // libdrm lists the directory to find the GPUs (without it RADV sees no device); only the render nodes in it are writable. (A card node opens read only at most, and has no ioctl right.)
    ro.emplace_back("/dev/dri");
    appendGlob(rw, "/dev/dri/renderD*"); // render nodes only: no card node, no display control
    appendGlob(rw, "/dev/nvidia*");
    for (const char *path : {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random"})
        rw.emplace_back(path);
    rw.push_back(murasakiCache(env) + "/puppet-shader-cache");
    rw.push_back(murasakiCache(env) + "/puppet-gpu-cache");
    return policy;
}

std::vector<std::pair<std::string, std::string>> sceneProfileEnvironment(const ProfileEnvironment &env)
{
    const std::string gpuCache = murasakiCache(env) + "/gpu-cache";
    return {{"MESA_SHADER_CACHE_DIR", gpuCache},
            {"MESA_GLSL_CACHE_DIR", gpuCache},
            {"__GL_SHADER_DISK_CACHE_PATH", gpuCache}};
}

std::vector<std::pair<std::string, std::string>> puppetProfileEnvironment(const ProfileEnvironment &env)
{
    const std::string gpuCache = murasakiCache(env) + "/puppet-gpu-cache";
    return {{"MESA_SHADER_CACHE_DIR", gpuCache},
            {"MESA_GLSL_CACHE_DIR", gpuCache},
            {"__GL_SHADER_DISK_CACHE_PATH", gpuCache}};
}

void preparePuppetProfile(const ProfileEnvironment &env)
{
    mkdirs(murasakiCache(env) + "/puppet-shader-cache");
    mkdirs(murasakiCache(env) + "/puppet-gpu-cache");
}

void prepareSceneProfile(const ProfileEnvironment &env)
{
    mkdirs(murasakiCache(env) + "/shader-cache");
    mkdirs(murasakiCache(env) + "/gpu-cache");
}

Policy importProfile(const ImportProfileInput &input)
{
    Policy policy;
    policy.denyTcp = true;
    for (const char *system : {"/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/etc/localtime", "/dev/urandom", "/dev/null", "/proc/self"})
        policy.readOnly.emplace_back(system);
    // The helper and the libraries its own RUNPATH names (a Qt outside /usr), found the way the scene profile finds an engine's.
    appendProgramPaths(policy.readOnly, input.program, ProfileEnvironment());
    for (const std::string &source : input.sources)
        policy.readOnly.push_back(source);
    policy.readWrite.push_back(input.outDir);
    return policy;
}

Policy screenmatchProfile(const ScreenmatchProfileInput &input)
{
    Policy policy;
    policy.denyTcp = true;
    for (const char *system : {"/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/pipewire", "/dev/urandom", "/dev/null", "/proc/self", "/proc/cpuinfo", "/proc/meminfo"})
        policy.readOnly.emplace_back(system);
    appendProgramPaths(policy.readOnly, input.program, ProfileEnvironment(), false); // (the helper's own file and libraries, not an engine's tree)
    return policy; // (nothing is writable)
}

std::vector<std::pair<std::string, std::string>> screenmatchProfileEnvironment()
{
    return {{"PATH", "/usr/bin:/bin"}, {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}};
}

std::vector<std::pair<std::string, std::string>> importProfileEnvironment()
{
    return {{"PATH", "/usr/bin:/bin"}, {"LANG", "C.UTF-8"}, {"LC_ALL", "C.UTF-8"}, {"QT_QPA_PLATFORM", "offscreen"}};
}

} // namespace murasaki::sandbox
