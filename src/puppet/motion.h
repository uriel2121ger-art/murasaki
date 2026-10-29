#pragma once

#include "puppet/params.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// Motions (docs/pets/puppets.md, section 3.2): curves of a parameter, a part's opacity or the model over time, read from a `*.motion3.json`, evaluated with the four segment kinds,
// played with fades, looped, and scheduled by priority. The format fixes the segment codes and the time of a Bezier's inner control points but not how a value is found; the choices
// below (section 9, Q-F1 to Q-F3) are ours.
enum class SegmentKind { Linear = 0, Bezier = 1, Stepped = 2, InverseStepped = 3 };
enum class CurveTarget { Model, Parameter, PartOpacity };

struct Point {
    double t = 0; // seconds
    double v = 0;
};

// A segment starts at points[first] and ends at points[first + 1] (points[first + 3] for a Bezier, whose inner control points are first + 1 and first + 2).
struct Segment {
    SegmentKind kind = SegmentKind::Linear;
    std::uint32_t first = 0;
};

struct Curve {
    CurveTarget target = CurveTarget::Parameter;
    std::string id;
    std::vector<Point> points;
    std::vector<Segment> segments;
    std::optional<double> fadeIn;  // seconds
    std::optional<double> fadeOut;
};

struct Motion {
    double duration = 0; // seconds
    double fps = 30;
    bool loop = false;
    std::optional<double> fadeIn;
    std::optional<double> fadeOut;
    std::vector<Curve> curves;
};

struct MotionResult {
    bool ok = false;
    Motion motion;
    std::string error; // no file content in it
};

// Reads and validates a motion file: Version 3, bounded counts (puppetlimits.h), finite numbers, times from 0 up, never decreasing along a curve and along a Bezier's four points,
// segment codes 0 to 3, a flat Segments list that ends exactly after a segment. The counts in Meta are not trusted: the real ones are counted (and must be within the limits).
MotionResult parseMotion(std::string_view text);

// Value of a curve at time t (seconds): before the first point the first value, after the last the last value. Linear interpolates; Stepped keeps the segment's first value until
// its end; InverseStepped takes the segment's last value as soon as time is past its start; Bezier solves the cubic's time for the parameter (the control times are monotone,
// so the solution is unique) and evaluates its value. A NaN t is treated as 0 (an infinite one is before the first or after the last point). A curve without points gives 0.
double evaluateCurve(const Curve &curve, double t);

inline constexpr double kDefaultFadeSeconds = 1.0; // when neither the manifest nor the file say (ours)

// 0 .. 1 easing used by every fade: slow at both ends.
double fadeEase(double x);

// What the "Model" curves of a motion say, after weighting.
struct ModelValues {
    double opacity = 1;
    std::optional<double> eyeBlink; // the value of an EyeBlink curve, when a motion has one
    std::optional<double> lipSync;
};

// One motion being played.
class MotionPlayer
{
public:
    // fadeIn / fadeOut: seconds; negative means "use the file's, then the default". `loop` overrides the file's flag when set.
    MotionPlayer(std::shared_ptr<const Motion> motion, double fadeIn, double fadeOut, std::optional<bool> loop = std::nullopt);

    void update(double dt); // a negative or non-finite step is ignored
    // Starts the fade out now (a loop ends, a motion stops early); the player is finished when it reaches zero.
    void stop();
    bool finished() const { return m_finished; }
    bool stopping() const { return m_stopping; }
    double time() const { return m_time; }       // position in the motion
    double weight() const;                         // fade in x fade out, 0 .. 1
    // Blends the curves into the parameters and parts: each value moves from its current one toward the curve's by the weight (the curve's own fades included).
    void apply(ParameterSet &parameters, PartSet &parts, ModelValues &model) const;

private:
    double curveWeight(const Curve &curve) const;

    std::shared_ptr<const Motion> m_motion;
    double m_fadeIn;
    double m_fadeOut;
    bool m_loop;
    double m_age = 0;  // since it started: drives the fade in
    double m_time = 0; // position in the motion
    bool m_stopping = false;
    double m_stopAge = 0; // seconds since stop()
    bool m_finished = false;
};

enum class Priority { None = 0, Idle = 1, Normal = 2, Force = 3 };

// The scheduler of a puppet's motions. At most one motion is "current"; the ones it replaced fade out beside it. Rules (ours):
//  - Force starts at once: the queue is dropped and everything playing fades out.
//  - Normal replaces a playing Idle (which fades out); behind a playing Normal or Force it waits in the queue (at most kMaxMotionQueue, a request over that is refused).
//  - Idle starts only when nothing plays and nothing waits, and is refused otherwise.
//  - When the current motion has finished (its fade out included) the first queued one starts.
class MotionManager
{
public:
    struct Request {
        std::shared_ptr<const Motion> motion;
        Priority priority = Priority::Normal;
        double fadeIn = -1;
        double fadeOut = -1;
        std::string tag; // the host's name for it (a group and an index), returned by currentTag()
    };

    bool request(Request request); // true: started or queued
    void update(double dt);
    void apply(ParameterSet &parameters, PartSet &parts, ModelValues &model) const;
    bool playing() const { return m_current.has_value(); }
    Priority currentPriority() const { return m_current ? m_current->priority : Priority::None; }
    const std::string &currentTag() const;
    size_t queued() const { return m_queue.size(); }
    size_t fadingOut() const { return m_fading.size(); }
    void clear();

private:
    struct Active {
        MotionPlayer player;
        Priority priority;
        std::string tag;
    };
    void startNow(Request &&request);
    void retireCurrent();

    std::optional<Active> m_current;
    std::vector<Active> m_fading;
    std::vector<Request> m_queue;
};

} // namespace murasaki::puppet
