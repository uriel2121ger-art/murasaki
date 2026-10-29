#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace murasaki {

struct PlaybackInput {
    QString output;
    QString itemId; // empty when the output shows nothing
    bool muted = false;
    bool occluded = false;
};

struct PlaybackDecision {
    bool paused = false;
    bool audible = false;

    bool operator==(const PlaybackDecision &other) const = default;
};

// Covered outputs pause (unless pauseWhenCovered is off). Among the unpaused, unmuted outputs showing the same
// item, only the first by connector name is audible, so the same video on two monitors is heard once.
QHash<QString, PlaybackDecision> decidePlayback(const QList<PlaybackInput> &inputs, bool pauseWhenCovered = true);

struct SpectrumInput {
    QString kind; // WallpaperContent::kind of what the output shows
    bool paused = false;
    bool wantsAudio = false; // the page on screen registered an audio listener
};

// Only a running web page that listens for audio is fed the spectrum.
bool outputWantsSpectrum(const QString &kind, bool paused, bool wantsAudio);
// The capture stream is needed iff some output wants the spectrum.
bool anyWantsSpectrum(const QList<SpectrumInput> &outputs);

} // namespace murasaki
