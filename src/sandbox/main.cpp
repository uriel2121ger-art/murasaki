// murasaki-sandbox: confines a program with Landlock, then replaces itself with it.
//
//   murasaki-sandbox [--ro PATH]... [--rw PATH]... [--no-net] [--require] [--abi N] [--verbose] -- PROGRAM [ARGS...]
//   murasaki-sandbox --profile scene --item DIR [--assets DIR] [--require] [--bwrap PATH|none] -- ENGINE [ARGS...]
//   murasaki-sandbox --profile import --src PATH... --out DIR [--require] -- murasaki-petimport [ARGS...]
//   murasaki-sandbox --profile puppet --item DIR [--core FILE] [--require] [--bwrap PATH|none] -- murasaki-puppet --engine [ARGS...]
//   murasaki-sandbox --profile screenmatch [--require] -- murasaki-screenmatch [ARGS...]
//   murasaki-sandbox --report
//
// The scene profile is the policy of the scene engines (see profile.h); the other options add to it. With the profile,
// and when bubblewrap is installed and works, the engine first gets a mount view of its own (bwrap.h: nothing but the
// policy's files and the audio sockets, no network, no session bus) and Landlock is applied inside it; without bwrap
// Landlock alone applies, and says so once. --report prints what this system offers, as one line of JSON.
//
// The program can read (and execute) what is under the --ro paths, read and write under the --rw paths, and nothing
// else on the filesystem. With --no-net it can not use TCP. On kernels with Landlock ABI 6 or newer it can not
// connect to abstract unix sockets or signal processes outside the sandbox either. File descriptors that are open
// at exec (the bridge socket) keep working. Without Landlock the helper says so once on stderr and runs the program
// unconfined, unless --require is given (then it exits with status 125).
//
// No Qt, no dynamic allocation after the policy is built: this runs in front of every scene engine.

#include "bwrap.h"
#include "landlock.h"
#include "profile.h"
#include "seccomp.h"

#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kExitUsage = 125;
constexpr int kExitNotFound = 127;

int usage(const char *problem)
{
    if (problem)
        std::fprintf(stderr, "murasaki-sandbox: %s\n", problem);
    std::fprintf(stderr,
                 "usage: murasaki-sandbox [--ro PATH]... [--rw PATH]... [--no-net] [--require] [--abi N] [--verbose]"
                 " [--profile scene --item DIR [--assets DIR] [--bwrap PATH|none]] -- PROGRAM [ARGS...]\n"
                 "       murasaki-sandbox --profile import --src PATH... --out DIR [--require] -- PROGRAM [ARGS...]\n"
                 "       murasaki-sandbox --profile puppet --item DIR [--core FILE] [--require] [--bwrap PATH|none] -- PROGRAM [ARGS...]\n"
                 "       murasaki-sandbox --profile screenmatch [--require] -- PROGRAM [ARGS...]\n"
                 "       murasaki-sandbox --report\n");
    return kExitUsage;
}

} // namespace

extern char **environ;

int main(int argc, char **argv)
{
    murasaki::sandbox::Policy policy;
    murasaki::sandbox::SceneProfileInput scene;
    bool sceneProfile = false;
    bool importProfile = false;
    bool puppetProfile = false;
    bool screenmatchProfile = false;
    std::string corePath;
    murasaki::sandbox::ImportProfileInput import;
    std::string runtimeDir;
    bool require = false;
    bool verbose = false;
    bool report = false;
    bool noBwrap = false;
    bool usePuppetPolicy = false;
    bool insideBwrap = false; // set by ourselves when we run again inside the namespace
    std::string bwrapPreference;
    int program = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            program = i + 1;
            break;
        }
        if (arg == "--ro" || arg == "--rw" || arg == "--abi" || arg == "--profile" || arg == "--item" || arg == "--assets" || arg == "--bwrap" || arg == "--src" || arg == "--out" || arg == "--core") {
            if (i + 1 >= argc)
                return usage("missing value");
            const std::string value = argv[++i];
            if (arg == "--ro") {
                policy.readOnly.push_back(value);
            } else if (arg == "--rw") {
                policy.readWrite.push_back(value);
            } else if (arg == "--profile") {
                if (value != "scene" && value != "import" && value != "puppet" && value != "screenmatch")
                    return usage("the profiles are scene, import, puppet and screenmatch");
                (value == "scene" ? sceneProfile : value == "puppet" ? puppetProfile : value == "screenmatch" ? screenmatchProfile : importProfile) = true;
            } else if (arg == "--core") {
                corePath = value;
            } else if (arg == "--src") {
                import.sources.push_back(value);
            } else if (arg == "--out") {
                import.outDir = value;
            } else if (arg == "--bwrap") {
                bwrapPreference = value;
            } else if (arg == "--item") {
                scene.itemDir = value;
            } else if (arg == "--assets") {
                scene.assetsDir = value;
            } else {
                char *end = nullptr;
                const long abi = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || *end != '\0' || abi < 0 || abi > 100)
                    return usage("--abi needs a number from 0 to 100");
                policy.maxAbi = static_cast<int>(abi);
            }
        } else if (arg == "--no-net") {
            policy.denyTcp = true;
        } else if (arg == "--report") {
            report = true;
        } else if (arg == "--no-bwrap") {
            noBwrap = true;
        } else if (arg == "--inside-bwrap") {
            insideBwrap = noBwrap = true;
        } else if (arg == "--require") {
            require = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else {
            return usage(("unknown option " + arg).c_str());
        }
    }
    if (report) {
        const std::string bwrap = murasaki::sandbox::findBwrap(bwrapPreference);
        const bool bwrapOk = !bwrap.empty() && murasaki::sandbox::bwrapWorks(bwrap);
        const int abi = murasaki::sandbox::kernelAbi();
        // unixSocketsHidden: the session bus, Wayland and X11 sockets are not reachable (a mount namespace; Landlock alone, ABI 8, can not refuse them).
        std::printf("{\"landlockAbi\":%d,\"tcpDeny\":%s,\"scopes\":%s,\"bwrap\":%s,\"unixSocketsHidden\":%s}\n", abi, abi >= 4 || bwrapOk ? "true" : "false",
                    abi >= 6 ? "true" : "false", bwrapOk ? "true" : "false", bwrapOk ? "true" : "false");
        return 0;
    }
    if (program < 0 || program >= argc)
        return usage("missing -- PROGRAM");

    // The program as an absolute path: the namespace has other working directory and PATH directories than we do.
    std::string programPath = argv[program];
    if (sceneProfile || puppetProfile || screenmatchProfile) {
        if (programPath.find('/') == std::string::npos) {
            const char *path = std::getenv("PATH");
            const std::string dirs = path ? path : "/usr/bin:/bin";
            size_t pos = 0;
            while (pos <= dirs.size()) {
                const size_t next = dirs.find(':', pos);
                const std::string dir = dirs.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                const std::string candidate = (dir.empty() ? "." : dir) + "/" + programPath;
                if (::access(candidate.c_str(), X_OK) == 0) {
                    programPath = candidate;
                    break;
                }
                if (next == std::string::npos)
                    break;
                pos = next + 1;
            }
        }
        char absolute[PATH_MAX];
        if (programPath.find('/') != std::string::npos && ::realpath(programPath.c_str(), absolute) && programPath[0] != '/')
            programPath = absolute;
        argv[program] = programPath.data();
    }

    if (puppetProfile) {
        // A puppet engine is a GPU program of untrusted data: the scene profile's machinery (item, GPU, caches, mount namespace) with the puppet policy, its own limits and no parent to outlive.
        if (sceneProfile || importProfile)
            return usage("--profile puppet takes no other profile");
        if (!corePath.empty() && corePath[0] != '/')
            return usage("--core needs an absolute path");
        sceneProfile = true; // (the common path below)
        usePuppetPolicy = true;
        const rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore);
        const pid_t parent = ::getppid();
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent)
            return kExitUsage;
    }
    if (screenmatchProfile) {
        // The helper that watches the screen: the least of everything (profile.h), its own limits and environment, and no parent to outlive. It loads its seccomp filter itself, first thing.
        if (sceneProfile || importProfile || puppetProfile)
            return usage("--profile screenmatch takes no other profile");
        if (!policy.readOnly.empty() || !policy.readWrite.empty() || !import.sources.empty() || !import.outDir.empty() || !scene.itemDir.empty() || !corePath.empty())
            return usage("--profile screenmatch takes no paths");
        murasaki::sandbox::ScreenmatchProfileInput input;
        input.program = argv[program];
        murasaki::sandbox::Policy base = murasaki::sandbox::screenmatchProfile(input);
        base.maxAbi = policy.maxAbi;
        policy = std::move(base);
        const auto limit = [](int resource, rlim_t value) {
            const rlimit l{value, value};
            ::setrlimit(resource, &l);
        };
        limit(RLIMIT_AS, 4ULL << 30);
        limit(RLIMIT_CORE, 0);
        limit(RLIMIT_NOFILE, 256);
        ::clearenv();
        for (const auto &[name, value] : murasaki::sandbox::screenmatchProfileEnvironment())
            ::setenv(name.c_str(), value.c_str(), 1);
        const pid_t parent = ::getppid();
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent)
            return kExitUsage;
    }
    if (importProfile) {
        if (sceneProfile || import.outDir.empty() || import.outDir[0] != '/')
            return usage("--profile import needs --out DIR (absolute) and no other profile");
        for (const std::string &source : import.sources) {
            if (source.empty() || source[0] != '/')
                return usage("--src needs absolute paths");
        }
        import.program = argv[program];
        murasaki::sandbox::Policy base = murasaki::sandbox::importProfile(import);
        // Nothing but the profile: --ro and --rw next to it would be a hole in the one reviewed place.
        if (!policy.readOnly.empty() || !policy.readWrite.empty())
            return usage("--profile import takes --src and --out, not --ro or --rw");
        base.maxAbi = policy.maxAbi;
        policy = std::move(base);
        // The limits and the environment of a job. The helper limits itself as well; these hold even if it did not.
        const auto limit = [](int resource, rlim_t value) {
            const rlimit l{value, value};
            ::setrlimit(resource, &l);
        };
        limit(RLIMIT_AS, 4ULL << 30);
        limit(RLIMIT_CPU, 60 * 60); // (the threads of a video decoder add up; the job's own time budget ends it long before)
        limit(RLIMIT_CORE, 0);
        limit(RLIMIT_NOFILE, 256);
        limit(RLIMIT_FSIZE, 4ULL << 30);
        ::clearenv();
        for (const auto &[name, value] : murasaki::sandbox::importProfileEnvironment())
            ::setenv(name.c_str(), value.c_str(), 1);
        // The job dies with whoever started it (the daemon): no orphan keeps decoding for nobody.
        const pid_t parent = ::getppid();
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent) // it went away between the two calls
            return kExitUsage;
    }

    if (sceneProfile) {
        if (scene.itemDir.empty())
            return usage("--profile scene and --profile puppet need --item");
        murasaki::sandbox::ProfileEnvironment env;
        const auto get = [](const char *name) { const char *v = std::getenv(name); return v ? std::string(v) : std::string(); };
        env.home = get("HOME");
        env.xdgCacheHome = get("XDG_CACHE_HOME");
        env.xdgConfigHome = get("XDG_CONFIG_HOME");
        env.xdgRuntimeDir = get("XDG_RUNTIME_DIR");
        runtimeDir = env.xdgRuntimeDir;
        // Relative values are not paths of ours (the XDG rule); without a home there is no place for caches.
        if (env.home.empty() || env.home[0] != '/')
            return usage("HOME is not set to an absolute path");
        for (std::string *dir : {&env.xdgCacheHome, &env.xdgConfigHome, &env.xdgRuntimeDir}) {
            if (!dir->empty() && (*dir)[0] != '/')
                dir->clear();
        }
        scene.program = argv[program];
        murasaki::sandbox::Policy base;
        if (usePuppetPolicy) {
            murasaki::sandbox::PuppetProfileInput puppet;
            puppet.itemDir = scene.itemDir;
            puppet.corePath = corePath;
            puppet.program = scene.program;
            base = murasaki::sandbox::puppetProfile(puppet, env);
        } else {
            base = murasaki::sandbox::sceneProfile(scene, env);
        }
        base.readOnly.insert(base.readOnly.end(), policy.readOnly.begin(), policy.readOnly.end());
        base.readWrite.insert(base.readWrite.end(), policy.readWrite.begin(), policy.readWrite.end());
        base.denyTcp = base.denyTcp || policy.denyTcp;
        base.maxAbi = policy.maxAbi;
        policy = std::move(base);
        if (usePuppetPolicy)
            murasaki::sandbox::preparePuppetProfile(env);
        else
            murasaki::sandbox::prepareSceneProfile(env);
        for (const auto &[name, value] : usePuppetPolicy ? murasaki::sandbox::puppetProfileEnvironment(env) : murasaki::sandbox::sceneProfileEnvironment(env))
            ::setenv(name.c_str(), value.c_str(), /*overwrite=*/0);
    }

    if ((sceneProfile || importProfile || screenmatchProfile) && !noBwrap) {
        const std::string bwrap = murasaki::sandbox::findBwrap(bwrapPreference);
        if (!bwrap.empty() && murasaki::sandbox::bwrapWorks(bwrap)) {
            char self[PATH_MAX];
            const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
            if (n > 0) {
                murasaki::sandbox::BwrapInput input;
                input.bwrap = bwrap;
                input.helper = std::string(self, static_cast<size_t>(n));
                input.runtimeDir = runtimeDir; // (an import has none: no runtime directory, so no bus, Wayland or X11 socket exists in its view)
                input.innerArguments.push_back("--inside-bwrap");
                for (int i = 1; i < argc; ++i)
                    input.innerArguments.push_back(argv[i]);
                std::vector<std::string> command = murasaki::sandbox::bwrapCommand(
                    policy, input, importProfile ? murasaki::sandbox::importProfileEnvironment() : screenmatchProfile ? murasaki::sandbox::screenmatchProfileEnvironment() : murasaki::sandbox::keptEnvironment(environ));
                std::vector<char *> raw;
                for (std::string &part : command)
                    raw.push_back(part.data());
                raw.push_back(nullptr);
                ::execv(bwrap.c_str(), raw.data());
                std::fprintf(stderr, "murasaki-sandbox: cannot run %s: %s\n", bwrap.c_str(), std::strerror(errno));
            }
        }
        std::fprintf(stderr, "murasaki-sandbox: no mount namespace (bwrap is missing or not allowed here), Landlock only\n");
    }
    // Landlock alone can not refuse a connect to a pathname unix socket (the session bus, Wayland, X11: ABI 9 adds a right for it, which is not used here). The
    // import helper decodes hostile files, so under --require it runs only in the mount namespace, where those sockets are not there.
    if (importProfile && require && !insideBwrap) {
        std::fprintf(stderr, "murasaki-sandbox: cannot sandbox: the session's unix sockets can not be hidden (no mount namespace)\n");
        return kExitUsage;
    }

    const murasaki::sandbox::Result result = murasaki::sandbox::apply(policy);
    // The network is refused either by Landlock (TCP rules) or by the namespace we are in.
    const bool tcpRefused = result.netDenied || insideBwrap;
    if (result.applied && require && policy.denyTcp && !tcpRefused) {
        std::fprintf(stderr, "murasaki-sandbox: cannot sandbox: TCP can not be refused (Landlock ABI %d, no network namespace)\n",
                     result.abi);
        return kExitUsage;
    }
    if (!result.applied) {
        if (require) {
            std::fprintf(stderr, "murasaki-sandbox: cannot sandbox: %s\n", result.error.c_str());
            return kExitUsage;
        }
        // One line, once: the program still runs, as it did before the sandbox existed.
        std::fprintf(stderr, "murasaki-sandbox: running %s unconfined: %s\n", argv[program], result.error.c_str());
    } else if (verbose) {
        std::fprintf(stderr, "murasaki-sandbox: Landlock ABI %d, tcp %s, scopes %s\n", result.abi,
                     result.netDenied ? "denied" : "not restricted", result.scoped ? "on" : "off");
        for (const std::string &path : result.skipped)
            std::fprintf(stderr, "murasaki-sandbox: skipped %s (not there)\n", path.c_str());
    }

    // The puppet engine also loses the kernel interfaces a GPU program of hostile data never needs (they fail with EPERM).
    if (usePuppetPolicy) {
        std::string seccompError;
        if (!murasaki::sandbox::applyPuppetSeccomp(seccompError)) {
            if (require) {
                std::fprintf(stderr, "murasaki-sandbox: cannot sandbox: %s\n", seccompError.c_str());
                return kExitUsage;
            }
            std::fprintf(stderr, "murasaki-sandbox: running %s without a syscall filter: %s\n", argv[program], seccompError.c_str());
        }
    }

    ::execvp(argv[program], argv + program);
    std::fprintf(stderr, "murasaki-sandbox: cannot run %s: %s\n", argv[program], std::strerror(errno));
    return kExitNotFound;
}
