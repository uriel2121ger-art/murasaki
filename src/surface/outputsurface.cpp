#include "outputsurface.h"

#include "shellwindows.h"
#include "wallpaperroot.h"

#include <QGuiApplication>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScreen>

#ifdef MURASAKI_HAVE_LAYERSHELL
#include <LayerShellQt/window.h>
#endif
#ifdef MURASAKI_HAVE_X11
#include "x11hints.h"
#endif

using namespace Qt::StringLiterals;

namespace murasaki {

OutputSurface::OutputSurface(QScreen *screen, QQmlEngine *engine, QObject *webProfile, bool windowed, WallpaperLayer wallpaperLayer)
    : QQuickView(engine, nullptr)
    , m_name(screen->name())
    , m_windowed(windowed)
{
    setColor(Qt::black);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setScreen(screen);
    if (windowed) {
        setTitle(u"Murasaki — "_s + m_name);
        resize(screen->size().scaled(960, 960, Qt::KeepAspectRatio));
    } else if (useShellWindows() && !QGuiApplication::platformName().startsWith(u"xcb"_s)) {
        // GNOME: no layer-shell. An ordinary frameless window the Murasaki extension of the shell recognises by its
        // title and pins as the desktop background (docs/gnome.md).
        setTitle(shellWallpaperTitle(m_name));
        setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        setGeometry(screen->geometry());
    } else {
        setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        setGeometry(screen->geometry());
#ifndef MURASAKI_HAVE_LAYERSHELL
        Q_UNUSED(wallpaperLayer)
#endif
#ifdef MURASAKI_HAVE_X11
        // An X11 session has no layer-shell: the window is a desktop-type window below everything, on every desktop.
        if (QGuiApplication::platformName().startsWith(u"xcb"_s))
            murasaki::x11::applyDesktopHints(this); // creates the native window; the hints are set before it is mapped
#endif
#ifdef MURASAKI_HAVE_LAYERSHELL
        if (auto *layer = LayerShellQt::Window::get(this)) {
            using LayerWindow = LayerShellQt::Window;
            // Under every other client: the background layer where the compositor has none of its own to cover
            // (see compositorprofile.h); Plasma keeps the bottom one.
            layer->setLayer(wallpaperLayer == WallpaperLayer::Background ? LayerWindow::LayerBackground : LayerWindow::LayerBottom);
            layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop | LayerWindow::AnchorBottom
                                                   | LayerWindow::AnchorLeft | LayerWindow::AnchorRight));
            layer->setExclusiveZone(-1);
            layer->setKeyboardInteractivity(LayerWindow::KeyboardInteractivityNone);
            layer->setScope(u"murasaki-wallpaper"_s);
#if MURASAKI_LAYERSHELL_HAS_SCREEN
            layer->setScreen(screen);
#endif
        }
#endif
    }
    setSource(QUrl(u"qrc:/qt/qml/Murasaki/Render/Wallpaper.qml"_s));
    for (const QQmlError &error : errors())
        qWarning("murasaki-surface: %s", qPrintable(error.toString()));
    if (QQuickItem *root = rootObject()) {
        root->setProperty("webProfile", QVariant::fromValue(webProfile));
        root->setProperty("refreshRate", screen->refreshRate());
        root->setProperty("outputName", m_name);
        new wallpaperroot::HealthRelay(root, [this](const QString &key, const QString &event, const QString &reason, const QString &engine) {
            notifyHealth({key, event, reason, engine});
        });
        const QMetaProperty wants = root->metaObject()->property(root->metaObject()->indexOfProperty("wantsAudio"));
        if (wants.isValid() && wants.hasNotifySignal())
            connect(root, wants.notifySignal(), this, QMetaMethod::fromSignal(&OutputSurface::wantsAudioChanged));
    }
    connect(this, &OutputSurface::wantsAudioChanged, this, [this] { notifyWantsAudioChanged(); });
}

void OutputSurface::setContent(const QVariantMap &content)
{
    wallpaperroot::setContent(rootObject(), content);
}

void OutputSurface::setPlayback(bool paused, bool audible)
{
    wallpaperroot::setPlayback(rootObject(), paused, audible);
}

void OutputSurface::setFrameRateLimit(int fps)
{
    wallpaperroot::setFrameRateLimit(rootObject(), fps);
}

void OutputSurface::setPlaybackCap(int fps)
{
    wallpaperroot::setPlaybackCap(rootObject(), fps);
}

bool OutputSurface::wantsAudio() const
{
    return wallpaperroot::wantsAudio(rootObject());
}

void OutputSurface::setAudioSpectrum(const QList<float> &spectrum)
{
    wallpaperroot::setAudioSpectrum(rootObject(), spectrum);
}

std::optional<double> OutputSurface::videoPosition() const
{
    const double position = wallpaperroot::videoPosition(rootObject());
    return position >= 0 ? std::optional<double>(position) : std::nullopt;
}

void OutputSurface::seekVideo(double seconds)
{
    wallpaperroot::seekVideo(rootObject(), seconds);
}

void OutputSurface::setInteractive(bool interactive)
{
    if (m_windowed)
        return; // a normal window always takes input
    setFlag(Qt::WindowTransparentForInput, !interactive);
}

} // namespace murasaki
