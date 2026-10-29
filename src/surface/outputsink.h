#pragma once

#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

class QQmlEngine;
class QObject;
class QScreen;

namespace murasaki {

// The pixel of a render surface of `size` that a position given as a fraction (0..1 of each axis) falls on: clamped to the surface, nothing for a
// NaN or an empty surface. Shared by the host's cursor and pointer paths.
inline std::optional<QPoint> pixelOfFraction(double x, double y, QSize size)
{
    if (!(x == x) || !(y == y) || size.width() <= 0 || size.height() <= 0)
        return std::nullopt;
    const auto axis = [](double fraction, int extent) {
        return std::clamp(int(std::lround(std::clamp(fraction, 0.0, 1.0) * extent)), 0, extent - 1);
    };
    return QPoint(axis(x, size.width()), axis(y, size.height()));
}

// What Wallpaper.qml says about the health of what it shows: `event` is "failed" (reason and the engine that was
// running, empty for a video or a page) or "frame" (the content drew its first frame). `key` is the content's key.
struct SinkHealthEvent {
    QString key;
    QString event;
    QString reason;
    QString engine;
};

// Where SurfaceController puts the wallpaper of one output: a layer-shell window (OutputSurface), the offscreen
// render-control window of the Plasma host mode (HostOutput) or a fake in the tests. The controller decides what
// plays and how; a sink only shows it.
class OutputSink
{
public:
    virtual ~OutputSink() = default;

    virtual QString outputName() const = 0;
    virtual void setContent(const QVariantMap &content) = 0;
    virtual void setPlayback(bool paused, bool audible) = 0;
    // Animated wallpapers (web pages, scenes) draw at most this often (the daemon's webFpsLimit).
    virtual void setFrameRateLimit(int fps) = 0;
    // Power saving: no wallpaper (videos included, unlike setFrameRateLimit) draws or decodes more often than this; 0
    // lifts it.
    virtual void setPlaybackCap(int /*fps*/) {}
    // Web and scene wallpapers take the pointer; everything else lets it through.
    virtual void setInteractive(bool interactive) = 0;
    // The page on screen asked for audio (registered a listener). Changes after load: the sink reports it through
    // notifyWantsAudioChanged().
    virtual bool wantsAudio() const = 0;
    virtual void setAudioSpectrum(const QList<float> &spectrum) = 0;
    // The cursor is at this fraction (0..1 of each axis) of the output, as reported by the compositor. Used where
    // the desktop above the wallpaper takes the pointer events (the Plasma host); a sink that receives them
    // itself ignores it. Hover only: clicks belong to whatever is on top.
    virtual void setCursorFraction(double /*x*/, double /*y*/) {}
    // The user clicks on the wallpaper through the overlay's interaction mode (docs/dbus-api.md, "Interacting with the wallpaper"): the
    // pointer is at this fraction of the output, with these buttons held (bit 0 left, 1 right, 2 middle) and this wheel movement (in 1/8 degree
    // steps, 120 per notch). Presses seen here are real user presses. Only the Plasma host acts on it; a sink with a window of its own
    // gets the pointer itself.
    virtual void injectPointerFraction(double /*x*/, double /*y*/, unsigned /*buttons*/, int /*wheelX*/, int /*wheelY*/) {}

    // Spanning: the position in seconds of the video on screen, nothing when there is none (or it has not started),
    // and a seek of it. The controller keeps the outputs of one spanned video together with these.
    virtual std::optional<double> videoPosition() const { return std::nullopt; }
    virtual void seekVideo(double /*seconds*/) {}

    // The controller's hook for wantsAudio() changes.
    void setWantsAudioListener(std::function<void()> listener) { m_wantsAudioListener = std::move(listener); }
    // The controller's hook for what the wallpaper reports about itself (a failure, a first frame).
    void setHealthListener(std::function<void(const SinkHealthEvent &)> listener) { m_healthListener = std::move(listener); }

protected:
    void notifyWantsAudioChanged()
    {
        if (m_wantsAudioListener)
            m_wantsAudioListener();
    }
    void notifyHealth(const SinkHealthEvent &event)
    {
        if (m_healthListener)
            m_healthListener(event);
    }

private:
    std::function<void()> m_wantsAudioListener;
    std::function<void(const SinkHealthEvent &)> m_healthListener;
};

// What a sink factory gets: the output to cover (its QScreen when the compositor shows one) and the shared QML
// engine and web profile the controller owns.
struct SinkRequest {
    QString name;
    QScreen *screen = nullptr;
    QQmlEngine *engine = nullptr;
    QObject *webProfile = nullptr; // QQuickWebEngineProfile
};

// Null means the sink could not be created.
using SinkFactory = std::function<std::unique_ptr<OutputSink>(const SinkRequest &)>;

} // namespace murasaki
