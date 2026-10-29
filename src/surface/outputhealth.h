#pragma once

#include <QString>

#include <optional>

namespace murasaki {

// Why a wallpaper is not on screen although something was asked for.
enum class FailureKind {
    Refused, // no engine would start it: not installed, not allowed (spanning, sandbox), cannot be launched
    Crashed, // the engine started and died, and the restarts ran out
    NoFrame, // everything started but nothing was drawn within the time allowed
};

QString failureKindName(FailureKind kind); // "refused", "crashed", "noframe"

// What a delegate's failed(reason) text means. "first frame" is the surface's own no-frame report, the exit and crash
// wording is the bridge's and the web item's; anything else is a start that was refused.
FailureKind classifyFailure(const QString &reason);

struct OutputFailure {
    FailureKind kind = FailureKind::Refused;
    QString reason;
    QString engine;         // the label of the engine that was running when it failed, empty for videos and pages
    bool canTryOther = false; // kept in Outputs() for its clients; false with the Vulkan engine
    bool operator==(const OutputFailure &) const = default;
};

// The failure state of one output, for the content (key) it shows. Pure logic: the QML root detects, the controller
// feeds this and publishes what it says.
//   - a new key clears the failure;
//   - a report for another key is stale and ignored;
//   - the same failure twice is not news (report returns false: it is logged and announced once);
//   - a first frame that arrives late clears a no-frame failure (a slow scene is not a broken one), but not a crash.
class OutputHealth
{
public:
    // The content on screen changed. True when that cleared a failure.
    bool begin(const QString &key);
    // True when the state changed (log it and announce it).
    bool report(const QString &key, const OutputFailure &failure);
    // The content drew a frame. True when that cleared a failure.
    bool frame(const QString &key);
    // The user asked again: the failure goes, the key stays. True when there was one.
    bool retry();

    const std::optional<OutputFailure> &failure() const { return m_failure; }
    const QString &key() const { return m_key; }

private:
    QString m_key;
    std::optional<OutputFailure> m_failure;
};

} // namespace murasaki
