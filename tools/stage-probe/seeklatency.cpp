// murasaki-seeklatency FILE [COUNT]: how long libmpv takes to land an exact seek in FILE (the copy of a stage), which the span sync of the windows of a desktop stage
// has to allow for (docs/pets/stage.md). Opens the file looping with no output, then COUNT times (12 by default) seeks to a pseudo-random position with
// "absolute+exact" and waits for mpv's playback-restart; prints one line per seek and the median and the worst in milliseconds. Test tool, not installed.

#include <mpv/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: murasaki-seeklatency FILE [COUNT]\n");
        return 2;
    }
    const int count = argc > 2 ? std::max(1, std::atoi(argv[2])) : 12;
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return 1;
    mpv_set_option_string(mpv, "vo", "null");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "loop-file", "inf");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "no");
    if (mpv_initialize(mpv) < 0)
        return 1;
    const char *load[] = {"loadfile", argv[1], nullptr};
    mpv_command(mpv, load);
    using Clock = std::chrono::steady_clock;
    const auto waitRestart = [&](double seconds) {
        const auto end = Clock::now() + std::chrono::duration<double>(seconds);
        while (Clock::now() < end) {
            mpv_event *event = mpv_wait_event(mpv, 0.05);
            if (event->event_id == MPV_EVENT_PLAYBACK_RESTART)
                return true;
            if (event->event_id == MPV_EVENT_END_FILE)
                return false;
        }
        return false;
    };
    if (!waitRestart(30)) {
        std::fprintf(stderr, "the file did not start\n");
        return 1;
    }
    double duration = 0;
    mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    std::printf("duration %.2f s\n", duration);
    std::vector<double> latencies;
    unsigned seed = 12345;
    for (int i = 0; i < count; ++i) {
        seed = seed * 1103515245u + 12345u;
        const double target = duration * 0.05 + (duration * 0.85) * ((seed >> 8) % 1000) / 1000.0;
        const std::string text = std::to_string(target);
        const char *seek[] = {"seek", text.c_str(), "absolute+exact", nullptr};
        const auto start = Clock::now();
        mpv_command_async(mpv, 0, seek);
        if (!waitRestart(30))
            continue;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        latencies.push_back(ms);
        std::printf("seek to %.2f s: %.0f ms\n", target, ms);
    }
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        std::printf("median %.0f ms, worst %.0f ms (%zu seeks)\n", latencies[latencies.size() / 2], latencies.back(), latencies.size());
    }
    mpv_terminate_destroy(mpv);
    return 0;
}
