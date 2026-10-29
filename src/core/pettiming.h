#pragma once

#include <QList>

#include <vector>

namespace murasaki {

// How fast a pet's animation plays (docs/pets/timing.md). Three modes, chosen by the two fields of a placement:
// File (fps 0, speed 1): every frame shows as long as the file says, after the rule for tiny delays (petlimits.h).
// Fixed (fps > 0): every frame shows max(1, round(1000 / fps)) ms, which is how Anima Engine steps its pets.
// Speed (fps 0, speed != 1): the file's delays, divided by speed (2 plays twice as fast).
inline constexpr int kMaxPetFps = 120;
inline constexpr int kAnimaPetFps = 15; // Anima Engine's own default rate; the pets' default is kDefaultPetFps (petlimits.h)
inline constexpr double kMinPetSpeed = 0.25;
inline constexpr double kMaxPetSpeed = 4.0;

enum class PetTimingMode { File, Fixed, Speed };

struct PetTiming {
    int fps = 0;
    double speed = 1;

    PetTimingMode mode() const
    {
        return fps > 0 ? PetTimingMode::Fixed : (speed != 1 ? PetTimingMode::Speed : PetTimingMode::File);
    }
    bool operator==(const PetTiming &other) const = default;
};

// The milliseconds every frame shows at a fixed rate.
int fixedFrameMs(int fps);

// Decides which frame of an animation shows at a given time, from the wall clock: the animation never slows down,
// a frame that is not drawn in time is skipped. Times are milliseconds on any monotonic clock the caller keeps.
//
//  - setFrames() gives the file's delays; setTiming() changes the mode without touching the frames (a live change:
//    the frame on screen goes on, the next ones follow the new timing).
//  - setCap() limits how often the shown frame may change (frames per second, 0: no limit). A frame that is due
//    sooner waits for the cap, and the frames that were due meanwhile are skipped.
//  - update() returns the frame to show at `now` and when to ask again (-1: never, a still image or a pause).
// Pure logic, no Qt event loop: tested without a window.
class FrameClock
{
public:
    struct Step {
        int frame = 0;
        double dueMs = -1;
        long long loop = 0; // loops(now): how many times the animation wrapped since setFrames()
    };

    void setFrames(const QList<int> &fileDelaysMs, double now);
    void setTiming(PetTiming timing, double now);
    void setCap(int fps) { m_capFps = fps > 0 ? fps : 0; }
    void pause(double now);
    void resume(double now);

    Step update(double now);

    int frame() const { return m_shown; }
    int frameCount() const { return int(m_file.size()); }
    bool paused() const { return m_paused; }
    PetTiming timing() const { return m_timing; }
    int cap() const { return m_capFps; }
    // How long frame `index` shows under the current timing.
    double frameMs(int index) const;

    // The wraps of the loop (the audio of a pet starts on them). Counted from setFrames(), they never go back: a live timing change, a pause and
    // a resume keep the count where it was, and only the wall time of the next wrap moves. A still image (fewer than two frames) never wraps.
    //  - loops(now): the wraps so far (frozen while paused).
    //  - wrapTimeMs(n): the wall time the n-th wrap happens (or happened) under the current timing; n = loops(now) is the last one, +1 the next.
    //  - nextWrapMs(now): the wall time of the next wrap, -1 when there is none (still image, paused).
    long long loops(double now) const;
    double wrapTimeMs(long long loop) const;
    double nextWrapMs(double now) const;

private:
    void rebuild();
    double startOfShown() const;
    long long loopsAt(double time) const;

    std::vector<double> m_file;   // the file's delays
    std::vector<double> m_ends;   // end of each frame within the loop, under the timing
    double m_total = 0;           // length of the loop
    PetTiming m_timing;
    int m_capFps = 0;
    double m_origin = 0;          // the wall time loop position 0 had (or would have had)
    int m_shown = 0;
    double m_lastChange = 0;      // when the shown frame changed
    bool m_paused = false;
    double m_pausedAt = 0;
    long long m_loopsBefore = 0; // the wraps counted before the current origin
};

} // namespace murasaki
