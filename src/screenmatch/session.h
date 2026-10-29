#pragma once

// The matching session of murasaki-screenmatch: what the daemon configured (triggers, regions, references, pets), and a tick that turns the newest frame of each stream into the scores of
// every region. Pure of threads, timers, I/O and PipeWire: commands and ticks go in, reply lines come out, the clock is a parameter and the frames come from a FrameSource. The detection
// itself (streaks, grace, cooldown, sequencer) is not here: it is the daemon's (ScreenMatcher); this only says how well each region matches the reference of the step it was told to compare.

#include "framesource.h"
#include "protocol.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <array>
#include <vector>

namespace murasaki::screenmatch {

struct SessionOptions {
    bool captureAllowed = false;       // the daemon started the helper to take explicit captures (the editor's "Capture now")
    qint64 captureMinGapMs = 1000;     // at most one capture a second
};

class Session
{
public:
    Session(FrameSource *source, SessionOptions options = {});

    // One line from the daemon (without its newline). Returns the reply lines (without newline): `ok <word>`, or `err <word> <reason>`; a capture adds its picture lines.
    QList<QByteArray> handle(const QByteArray &line, qint64 nowMs);
    // One tick at `nowMs`: takes the newest frame of each stream the regions use, scores every region, releases the frames. Nothing when stopped.
    QList<QByteArray> tick(qint64 nowMs);

    bool running() const { return m_running; }
    bool quitRequested() const { return m_quit; }
    int ticksPerSecond() const { return m_rate; }
    int triggerCount() const { return int(m_triggers.size()); }
    qint64 ticks() const { return m_tick; }

private:
    struct Region {
        bool set = false;
        int stream = 0, x = 0, y = 0, w = 0, h = 0;
    };
    struct Trigger {
        QString id;
        ScreenMatchMode mode = ScreenMatchMode::Hash;
        std::array<Region, kMaxRegionsPerTrigger> regions{};
        std::array<std::array<ScreenPicture, kMaxSteps>, kMaxRegionsPerTrigger> refs{};
        std::array<std::array<bool, kMaxSteps>, kMaxRegionsPerTrigger> haveRef{};
        int want = 0;
    };

    Trigger *find(const QString &id);
    int totalRegions() const;
    QList<QByteArray> capture(const Command &command, qint64 nowMs);
    bool covered(const Region &region) const;

    FrameSource *m_source;
    SessionOptions m_options;
    std::vector<Trigger> m_triggers;
    QList<PetRect> m_pets;
    int m_rate = 5;
    bool m_running = false;
    bool m_watching = false;
    bool m_quit = false;
    qint64 m_tick = 0;
    qint64 m_lastCaptureMs = -1;
};

// The colours of a rectangle of a frame as RGB bytes, area-averaged to a longer side of at most 360 (the one picture the helper may send out). Empty when the rectangle is not inside the frame
// or is under 2 pixels a side.
QByteArray scaledRgbCrop(const Frame &frame, int x, int y, int width, int height, int *outWidth, int *outHeight);

} // namespace murasaki::screenmatch
