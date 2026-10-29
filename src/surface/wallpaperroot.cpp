#include "wallpaperroot.h"

#include <QMetaObject>
#include <QQuickItem>

using namespace Qt::StringLiterals;

namespace murasaki::wallpaperroot {

HealthRelay::HealthRelay(QQuickItem *root,
                         std::function<void(const QString &, const QString &, const QString &, const QString &)> callback)
    : QObject(root), m_callback(std::move(callback))
{
    const int signalIndex = root->metaObject()->indexOfSignal("health(QString,QString,QString,QString)");
    if (signalIndex >= 0)
        connect(root, root->metaObject()->method(signalIndex), this,
                metaObject()->method(metaObject()->indexOfSlot("onHealth(QString,QString,QString,QString)")));
}

void HealthRelay::onHealth(const QString &key, const QString &event, const QString &reason, const QString &engine)
{
    m_callback(key, event, reason, engine);
}

void setContent(QQuickItem *root, const QVariantMap &content)
{
    if (root)
        root->setProperty("content", content);
}

void setPlayback(QQuickItem *root, bool paused, bool audible)
{
    if (!root)
        return;
    root->setProperty("paused", paused);
    root->setProperty("audible", audible);
}

void setFrameRateLimit(QQuickItem *root, int fps)
{
    if (root)
        root->setProperty("frameRateLimit", fps);
}

void setPlaybackCap(QQuickItem *root, int fps)
{
    if (root)
        root->setProperty("fpsCap", fps);
}

bool wantsAudio(const QQuickItem *root)
{
    return root && root->property("wantsAudio").toBool();
}

void setAudioSpectrum(QQuickItem *root, const QList<float> &spectrum)
{
    if (!root)
        return;
    QVariantList values;
    values.reserve(spectrum.size());
    for (const float value : spectrum)
        values.append(double(value));
    root->setProperty("audioSpectrum", values);
}

double videoPosition(QQuickItem *root)
{
    if (!root)
        return -1;
    QVariant position;
    if (!QMetaObject::invokeMethod(root, "videoPosition", Q_RETURN_ARG(QVariant, position)))
        return -1;
    return position.isValid() ? position.toDouble() : -1;
}

void seekVideo(QQuickItem *root, double seconds)
{
    if (root)
        QMetaObject::invokeMethod(root, "seekVideo", Q_ARG(QVariant, seconds));
}

} // namespace murasaki::wallpaperroot
