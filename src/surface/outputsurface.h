#pragma once

#include "compositorprofile.h"
#include "outputsink.h"

#include <QList>
#include <QQuickView>
#include <QVariantMap>

class QScreen;

namespace murasaki {

// One wallpaper window per screen: a layer-shell surface on the bottom layer, or a normal window with --windowed.
class OutputSurface : public QQuickView, public OutputSink
{
    Q_OBJECT

public:
    OutputSurface(QScreen *screen, QQmlEngine *engine, QObject *webProfile, bool windowed,
                  WallpaperLayer layer = WallpaperLayer::Bottom);

    // OutputSink. Web wallpapers take the pointer like Wallpaper Engine's (hover, clicks).
    QString outputName() const override { return m_name; }
    void setContent(const QVariantMap &content) override;
    void setPlayback(bool paused, bool audible) override;
    void setFrameRateLimit(int fps) override;
    void setInteractive(bool interactive) override;
    bool wantsAudio() const override;
    void setPlaybackCap(int fps) override;
    void setAudioSpectrum(const QList<float> &spectrum) override;
    std::optional<double> videoPosition() const override;
    void seekVideo(double seconds) override;

signals:
    void wantsAudioChanged();

private:
    QString m_name;
    bool m_windowed;
};

} // namespace murasaki
