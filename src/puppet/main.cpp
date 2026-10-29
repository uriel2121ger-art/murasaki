// murasaki-puppet: the puppet engine's process (docs/pets/puppets.md).
//
//   murasaki-puppet --check FOLDER     one line of JSON on stdout (check.h), exit status 0 whenever a verdict was printed. No GPU, no window.
//   murasaki-puppet --engine --item DIR --file NAME.inp --murasaki-bridge-fd N --murasaki-size WxH [--fps N] [--seed N] [--device D]
//                                      draws the puppet and hands the pictures to the bridge consumer on descriptor N (engine.h; exit statuses there)
//   murasaki-puppet --version
//
// Exit status 2: a usage error. No Qt.

#include "puppet/check.h"

#ifdef MURASAKI_PUPPET_ENGINE
#include "puppet/engine.h"
#endif

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef MURASAKI_VERSION
#define MURASAKI_VERSION "0"
#endif

namespace {

bool number(const char *text, long long min, long long max, long long &out)
{
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v < min || v > max)
        return false;
    out = v;
    return true;
}

int usage()
{
    std::fputs("usage: murasaki-puppet --check FOLDER | --version\n"
#ifdef MURASAKI_PUPPET_ENGINE
               "       murasaki-puppet --engine --item DIR --file NAME.inp --murasaki-bridge-fd N --murasaki-size WxH [--fps N] [--seed N] [--device D]\n"
#endif
               ,
               stderr);
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("murasaki-puppet %s\n", MURASAKI_VERSION);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--check") == 0) {
        const murasaki::puppet::ManifestResult result = murasaki::puppet::checkItem(argv[2]);
        std::printf("%s\n", murasaki::puppet::checkJson(result, MURASAKI_VERSION).c_str());
        return 0;
    }
#ifdef MURASAKI_PUPPET_ENGINE
    if (argc >= 2 && std::strcmp(argv[1], "--engine") == 0) {
        murasaki::puppet::EngineOptions options;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc)
                return usage();
            const char *value = argv[++i];
            long long n = 0;
            if (arg == "--item") {
                options.itemDir = value;
            } else if (arg == "--file") {
                options.file = value;
            } else if (arg == "--device") {
                options.device = value;
            } else if (arg == "--murasaki-bridge-fd") {
                if (!number(value, 3, 1 << 20, n))
                    return usage();
                options.bridgeFd = static_cast<int>(n);
            } else if (arg == "--murasaki-size") {
                unsigned w = 0, h = 0;
                char extra = 0;
                if (std::sscanf(value, "%ux%u%c", &w, &h, &extra) != 2 || w == 0 || h == 0 || w > 16384 || h > 16384)
                    return usage();
                options.width = w;
                options.height = h;
            } else if (arg == "--fps") {
                if (!number(value, 1, 240, n))
                    return usage();
                options.fps = static_cast<int>(n);
            } else if (arg == "--seed") {
                if (!number(value, 0, (1LL << 62), n))
                    return usage();
                options.seed = static_cast<std::uint64_t>(n);
            } else {
                return usage();
            }
        }
        std::signal(SIGPIPE, SIG_IGN);
        return murasaki::puppet::runEngine(options);
    }
#endif
    return usage();
}
