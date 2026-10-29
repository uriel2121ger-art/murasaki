#pragma once

// Adaptive frame rate: when the GPU needs more of each frame interval than it should, the engine draws less often (60,
// 45, 30, 20 fps, never above what the consumer asked for) and goes back up when there is room, with hysteresis so that
// it does not flap. Pure logic: the engine feeds it the GPU time of each frame it drew.
//
// That time includes waiting for the GPU's other clients (another output's scene, the compositor while a window is dragged),
// which come and go in bursts: both controllers step down on the median of a window, not its mean, so a burst under half of
// it is not taken for the scene's cost; and a step up that is followed soon by a step down makes the next step up wait
// longer (twice, up to kMaxHold times), which a long stable time forgets.
// A step down goes straight to the level predicted to fit; a step up is one level at a time.

#include <deque>
#include <optional>
#include <vector>

namespace murasaki::scene {

// The back-off of the step up (see above), shared by both controllers.
class StepBackOff
{
public:
    static constexpr unsigned kMaxHold = 8;
    // A frame at the current level.
    void tick();
    void steppedUp() { m_sinceUp = 0; }
    // `upFrames` is the controller's ordinary wait: a fall within two of them of the last step up doubles the wait.
    void steppedDown(unsigned upFrames);
    // The frames to wait since the last step down before stepping up.
    unsigned wait(unsigned upFrames) const { return upFrames * m_hold; }
    bool waited(unsigned upFrames) const { return m_sinceDown >= wait(upFrames); }
    unsigned hold() const { return m_hold; }

private:
    unsigned m_hold = 1;
    unsigned long m_sinceUp = ~0ul / 2;
    unsigned long m_sinceDown = ~0ul / 2;
    unsigned m_upFrames = 0; // of the last fall, for the forgetting
};

class FpsController
{
public:
    struct Config {
        double downLoad = 0.85;        // GPU time / frame interval above which the rate steps down
        double upLoad = 0.55;          // load the next rate up would have: below this the rate steps up
        unsigned downFrames = 48;      // window (frames) whose median must be over downLoad before stepping down
        unsigned upFrames = 150;       // window whose mean must fit the rate up, and the least wait after a step down
        std::vector<int> ladder{60, 45, 30, 20};
    };

    explicit FpsController(int requestedFps) : FpsController(requestedFps, Config()) {}
    FpsController(int requestedFps, Config config);

    // The consumer asked for another rate (Fps message): the ladder is rebuilt below it and the rate restarts at it.
    void setRequested(int fps);

    int requested() const { return m_requested; }
    int current() const { return m_ladder[m_level]; }

    // The GPU time (ms) of a frame that was drawn at the current rate. Returns the new rate when it changed.
    std::optional<int> update(double gpuMs);

private:
    void rebuild();

    Config m_config;
    int m_requested = 30;
    std::vector<int> m_ladder; // descending: m_ladder[0] == m_requested
    size_t m_level = 0;
    std::deque<double> m_samples; // recent frames at the current rate, newest last
    StepBackOff m_backOff;
};

// Adaptive resolution: the engine draws at most `ceiling` (the user's render quality) pixels per output pixel, and less,
// down to kFloor of it, while the GPU needs more than `budget` of each frame interval. It goes back up when the
// next step up would still fit (approximately: the cost follows the pixel count, the square of the scale).
class ScaleController
{
public:
    struct Config {
        double budget = 0.25;          // share of the frame interval the GPU may take; 0 turns the controller off
        unsigned downFrames = 48;      // window whose median must be over the budget before stepping down
        unsigned upFrames = 240;       // window whose mean predicts the step up, and the least wait after a step down
        double upMargin = 0.7;         // the predicted load at the next step up must be under budget * upMargin
        std::vector<double> steps{1.0, 0.85, 0.7, 0.55, 0.5}; // fractions of the ceiling
    };

    explicit ScaleController(double ceiling) : ScaleController(ceiling, Config()) {}
    ScaleController(double ceiling, Config config);

    void setCeiling(double ceiling);
    double ceiling() const { return m_ceiling; }
    double current() const { return m_ceiling * m_config.steps[m_level]; }
    bool atFloor() const { return m_level + 1 >= m_config.steps.size(); }
    bool enabled() const { return m_config.budget > 0.0; }
    // Forget the recent frames (the frame rate changed, so the loads are not comparable).
    void reset() { m_samples.clear(); }

    // `load` is the GPU time of a drawn frame over the frame interval at the current rate; `atRequestedRate` is false
    // while the frame rate is held below what was asked (then the resolution does not go back up yet). Returns the new
    // scale when it changed.
    std::optional<double> update(double load, bool atRequestedRate = true);

private:
    Config m_config;
    double m_ceiling = 1.0;
    size_t m_level = 0;
    std::deque<double> m_samples;
    StepBackOff m_backOff;
};

} // namespace murasaki::scene
