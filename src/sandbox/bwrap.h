#pragma once

#include "landlock.h"

#include <string>
#include <utility>
#include <vector>

namespace murasaki::sandbox {

// The mount-namespace layer in front of Landlock, built on bubblewrap when it is installed and usable. It does what
// Landlock cannot: the engine sees only the files of the policy and a private runtime directory with nothing but the
// audio sockets in it, so the session bus (a pathname unix socket Landlock can not refuse) is simply not there; it also
// gets its own network, process and IPC namespaces.

// The bwrap to use: `preference` when it names an executable file, "none" for no bwrap at all, otherwise bwrap on PATH.
// Empty when there is none.
std::string findBwrap(const std::string &preference);

// Whether bwrap can create the namespaces here (some systems forbid unprivileged user namespaces): runs it once around
// `true`. Bounded: a bwrap that hangs counts as unusable.
bool bwrapWorks(const std::string &bwrap);

struct BwrapInput {
    std::string bwrap;
    std::string helper;       // this program, to run again inside (it applies Landlock there)
    std::string runtimeDir;   // $XDG_RUNTIME_DIR on the host; empty: none
    std::vector<std::string> innerArguments; // for the helper inside, after its own path
};

// The environment variables the engine keeps (everything else is cleared): locale, paths, the graphics drivers' and
// audio libraries' settings, Murasaki's own. No display, bus or agent variables. Name, value.
std::vector<std::pair<std::string, std::string>> keptEnvironment(char **environment);

// Whether a policy path is procfs (the /proc bwrap mounts itself) and not merely a name that starts with "/proc".
bool isProcfsPath(const std::string &path);

// The complete bwrap command line (program first). The binds follow the Landlock policy; /dev/dri only with its render
// nodes; the runtime directory is a fresh tmpfs holding the PipeWire socket and the PulseAudio directory when they exist.
std::vector<std::string> bwrapCommand(const Policy &policy, const BwrapInput &input,
                                      const std::vector<std::pair<std::string, std::string>> &environment);

} // namespace murasaki::sandbox
