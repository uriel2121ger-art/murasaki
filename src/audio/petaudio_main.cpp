// murasaki-petaudio: the process that plays the sounds of the desktop pets (docs/pets/audio.md), and, with --import, the one that turns a sound file into a
// pet audio asset. Neither mode links Qt.
//
//   murasaki-petaudio [--assets DIR] [--backend pipewire|paced|none] [--idle-exit SECONDS] [--latency-ms N]   commands on stdin, answers on stdout
//   murasaki-petaudio --import SOURCE OUTDIR [--name NAME] [--deadline-ms N]                                  one JSON line on stdout
//   murasaki-petaudio --selftest
//
// Environment: MURASAKI_AUDIO_BACKEND (same as --backend), MURASAKI_AUDIO_LOG (a file the player appends "start PET GEN INTENDED_NS ACTUAL_NS" to, for tests).

#include "audioimport.h"
#include "petaudioasset.h"
#include "petmixer.h"
#include "player.h"
#include "protocol.h"

#include <poll.h>
#include <sys/prctl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

using namespace murasaki;
using namespace murasaki::audio;

namespace {

std::atomic<bool> g_terminate{false};

void onSignal(int) { g_terminate = true; }

std::int64_t monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

std::string jsonEscape(const std::string &in)
{
    std::string out;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20 || c == 0x7f) {
            out += ' ';
        } else {
            out += char(c);
        }
    }
    return out;
}

int runImport(int argc, char **argv)
{
    // --import SOURCE OUTDIR [--name N] [--deadline-ms N]
    if (argc < 4) {
        std::fprintf(stderr, "usage: murasaki-petaudio --import SOURCE OUTDIR [--name NAME] [--deadline-ms N]\n");
        return 2;
    }
    const std::string source = argv[2], outDir = argv[3];
    std::string name = source.substr(source.find_last_of('/') == std::string::npos ? 0 : source.find_last_of('/') + 1);
    ImportSettings settings;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--name"))
            name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--deadline-ms"))
            settings.deadlineMs = std::atoll(argv[i + 1]);
        else
            return 2;
    }
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    ImportCallbacks callbacks;
    callbacks.cancelled = [] { return g_terminate.load(); };
    const ImportResult r = importAudio(source, outDir, name, settings, callbacks);
    std::string degraded = "[";
    for (std::size_t i = 0; i < r.degraded.size(); ++i)
        degraded += (i ? ",\"" : "\"") + jsonEscape(r.degraded[i]) + "\"";
    degraded += "]";
    std::printf("{\"ok\":%s,\"cancelled\":%s,\"reused\":%s,\"error\":\"%s\",\"asset\":\"%s\",\"durationMs\":%lld,\"bytes\":%lld,\"sourceFormat\":\"%s\","
                "\"sourceCodec\":\"%s\",\"sourceRate\":%d,\"channels\":%d,\"degraded\":%s}\n",
                r.ok ? "true" : "false", r.cancelled ? "true" : "false", r.reused ? "true" : "false", jsonEscape(r.error).c_str(), r.assetId.c_str(), r.durationMs,
                r.bytes, jsonEscape(r.sourceFormat).c_str(), jsonEscape(r.sourceCodec).c_str(), r.sourceRate, r.channels, degraded.c_str());
    std::fflush(stdout);
    return r.ok ? 0 : 1;
}

int runSelfTest()
{
    PetMixer mixer;
    PlayCommand c;
    c.pet = 1;
    c.generation = 1;
    c.clip = std::make_shared<VectorClip>(std::vector<std::int16_t>(48000 * 2, 8000));
    mixer.play(c);
    std::vector<float> out(480 * 2);
    float peak = 0;
    for (int i = 0; i < 10; ++i) {
        mixer.render(out.data(), 480, monotonicNs());
        for (float f : out)
            peak = std::max(peak, f);
    }
    return peak > 0.2f && peak < 0.3f ? 0 : 1;
}

class Server
{
public:
    Server(std::string assets, std::unique_ptr<Player> player, double idleSeconds, long long latencyMs)
        : m_assets(std::move(assets)), m_player(std::move(player)), m_idleSeconds(idleSeconds), m_latencyNs(latencyMs * 1'000'000)
    {
        if (const char *log = std::getenv("MURASAKI_AUDIO_LOG"))
            m_log = std::fopen(log, "a");
    }
    ~Server()
    {
        if (m_player)
            m_player->stop();
        if (m_log)
            std::fclose(m_log);
    }

    int run()
    {
        std::string error;
        const bool started = m_player && m_player->start(
            [this](float *out, std::size_t frames) {
                return m_mixer.render(out, frames, monotonicNs() + m_latencyNs + m_player->latencyNs());
            },
            &error);
        if (!started) {
            std::printf("unavailable %s\n", error.empty() ? "no player" : error.c_str());
            std::fflush(stdout);
            return 3;
        }
        std::printf("ready\n");
        std::fflush(stdout);
        m_lastActivity = monotonicNs();
        std::string pending;
        bool active = false;
        while (!g_terminate) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            const int timeoutMs = m_mixer.voices() > 0 ? 20 : 250;
            const int r = poll(&p, 1, timeoutMs);
            if (r < 0 && errno != EINTR)
                return 1;
            if (r > 0 && (p.revents & (POLLIN | POLLHUP))) {
                char buffer[512];
                const ssize_t n = read(STDIN_FILENO, buffer, sizeof buffer);
                if (n == 0)
                    return 0; // the overlay closed the pipe
                if (n < 0 && errno != EINTR && errno != EAGAIN)
                    return 1;
                if (n > 0) {
                    pending.append(buffer, std::size_t(n));
                    std::size_t nl;
                    while ((nl = pending.find('\n')) != std::string::npos) {
                        const std::string line = pending.substr(0, nl);
                        pending.erase(0, nl + 1);
                        if (!handle(line))
                            return 0;
                    }
                    if (pending.size() > kMaxLineBytes)
                        return 2; // a line that never ends
                }
            }
            if (m_player->dead()) {
                std::fprintf(stderr, "murasaki-petaudio: the audio stream ended; leaving so that a new player is started\n");
                std::printf("dead\n");
                std::fflush(stdout);
                return 4;
            }
            const bool want = m_mixer.active() > 0;
            if (want != active) {
                m_player->setActive(want);
                active = want;
            }
            announce();
            if (m_mixer.voices() > 0)
                m_lastActivity = monotonicNs(); // (only a sound keeps the player: the controller's other lines do not)
            else if (double(monotonicNs() - m_lastActivity) / 1e9 > m_idleSeconds) {
                std::printf("idle\n");
                std::fflush(stdout);
                return 0;
            }
        }
        return 0;
    }

private:
    bool handle(const std::string &line)
    {
        const auto command = parseCommand(line);
        if (!command) {
            std::fprintf(stderr, "murasaki-petaudio: refused a line of %zu bytes\n", line.size());
            std::printf("refused\n");
            std::fflush(stdout);
            _exit(2); // (the audio thread may be running: no destructors)
        }
        switch (command->kind) {
        case Command::Kind::Quit:
            return false;
        case Command::Kind::Stop:
            m_mixer.stop(command->pet);
            break;
        case Command::Kind::Cancel:
            m_mixer.cancelPending(command->pet);
            break;
        case Command::Kind::Pause:
            m_mixer.pause(command->pet);
            break;
        case Command::Kind::Resume:
            m_mixer.resume(command->pet);
            break;
        case Command::Kind::Gain:
            m_mixer.setGain(command->pet, float(command->gain) / 1000.0f);
            break;
        case Command::Kind::Master:
            m_mixer.setMaster(float(command->gain) / 1000.0f);
            break;
        case Command::Kind::Play:
            play(*command);
            break;
        }
        return true;
    }

    void play(const Command &c)
    {
        std::shared_ptr<MappedClip> clip;
        if (const auto it = m_cache.find(c.asset); it != m_cache.end()) {
            clip = it->second;
        } else {
            std::string error;
            clip = MappedClip::open(assetPath(m_assets, c.asset), &error);
            if (!clip) {
                std::printf("failed %d %u %s\n", c.pet, c.generation, error.c_str());
                std::fflush(stdout);
                return;
            }
            clip->prefetch(0, clip->frames()); // the kernel reads it ahead, so the audio thread meets cached pages
            evict();
            m_cache[c.asset] = clip;
        }
        PlayCommand p;
        p.pet = c.pet;
        p.generation = c.generation;
        p.clip = clip;
        p.startFrame = std::size_t(c.startFrame);
        p.gain = float(c.gain) / 1000.0f;
        p.loop = c.loop;
        p.atNs = c.atNs;
        m_intended[{c.pet, c.generation}].push_back(c.atNs);
        m_mixer.play(std::move(p));
        m_lastActivity = monotonicNs(); // a new sound: the idle time starts from here
    }

    // Clips nothing plays are dropped when the cache is large; one a voice holds stays alive through its own reference.
    void evict()
    {
        if (m_cache.size() < 24)
            return;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (it->second.use_count() == 1)
                it = m_cache.erase(it);
            else
                ++it;
        }
    }

    void announce()
    {
        for (const Event &e : m_mixer.takeEvents()) {
            const char *word = e.kind == Event::Kind::Started ? "started" : e.kind == Event::Kind::Ended ? "ended" : "dropped";
            std::printf("%s %d %u\n", word, e.pet, e.generation);
            if (e.kind == Event::Kind::Started && m_log) {
                long long intended = 0;
                if (const auto it = m_intended.find({e.pet, e.generation}); it != m_intended.end() && !it->second.empty()) {
                    intended = it->second.front();
                    it->second.pop_front();
                }
                std::fprintf(m_log, "start %d %u %lld %lld\n", e.pet, e.generation, intended, static_cast<long long>(e.timeNs));
                std::fflush(m_log);
            }
        }
        std::fflush(stdout);
        if (m_intended.size() > 512)
            m_intended.clear();
    }

    std::string m_assets;
    std::unique_ptr<Player> m_player;
    PetMixer m_mixer;
    std::unordered_map<std::string, std::shared_ptr<MappedClip>> m_cache;
    std::map<std::pair<int, unsigned>, std::deque<long long>> m_intended; // the moments asked for, per sound, in the order of the commands
    double m_idleSeconds;
    long long m_latencyNs;
    std::int64_t m_lastActivity = 0;
    FILE *m_log = nullptr;
};

std::string defaultAssets()
{
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::string(xdg) + "/murasaki/pets/audio";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/share/murasaki/pets/audio";
    return {};
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGTERM); // not left behind when the overlay or the daemon dies
    if (argc >= 2 && !std::strcmp(argv[1], "--import"))
        return runImport(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "--selftest"))
        return runSelfTest();
    std::string assets = defaultAssets();
    std::string backend = std::getenv("MURASAKI_AUDIO_BACKEND") ? std::getenv("MURASAKI_AUDIO_BACKEND") : "pipewire";
    double idle = 30;
    long long latencyMs = 0;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--assets") && more)
            assets = argv[++i];
        else if (!std::strcmp(argv[i], "--backend") && more)
            backend = argv[++i];
        else if (!std::strcmp(argv[i], "--idle-exit") && more)
            idle = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--latency-ms") && more)
            latencyMs = std::atoll(argv[++i]);
        else {
            std::fprintf(stderr, "murasaki-petaudio: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (assets.empty() || idle < 0.1 || latencyMs < -500 || latencyMs > 2000)
        return 2;
    std::unique_ptr<Player> player;
    if (backend == "pipewire")
        player = makePipeWirePlayer();
    else if (backend == "paced")
        player = makePacedPlayer();
    else if (backend != "none")
        return 2;
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    Server server(assets, std::move(player), idle, latencyMs);
    return server.run();
}
