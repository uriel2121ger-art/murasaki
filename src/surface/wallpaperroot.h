#pragma once

#include <QList>
#include <QObject>
#include <QtGlobal>
#include <QVariantMap>

#include <functional>

class QQuickItem;

namespace murasaki::wallpaperroot {

// Relays the root's health(key, event, reason, engine) signal to a callback. Parented to the root, so it goes with it.
class HealthRelay : public QObject
{
    Q_OBJECT

public:
    HealthRelay(QQuickItem *root, std::function<void(const QString &, const QString &, const QString &, const QString &)> callback);

private slots:
    void onHealth(const QString &key, const QString &event, const QString &reason, const QString &engine);

private:
    std::function<void(const QString &, const QString &, const QString &, const QString &)> m_callback;
};

// The properties of Wallpaper.qml's root that every sink drives, in one place so the layer-shell window and the
// host-mode window cannot drift apart.
void setContent(QQuickItem *root, const QVariantMap &content);
void setPlayback(QQuickItem *root, bool paused, bool audible);
void setFrameRateLimit(QQuickItem *root, int fps);
void setPlaybackCap(QQuickItem *root, int fps);
bool wantsAudio(const QQuickItem *root);
void setAudioSpectrum(QQuickItem *root, const QList<float> &spectrum);
// The position in seconds of the video on screen (a spanned wallpaper keeps its outputs together with these), and a
// seek of it. Negative: no video, or it has not started.
double videoPosition(QQuickItem *root);
void seekVideo(QQuickItem *root, double seconds);

} // namespace murasaki::wallpaperroot
