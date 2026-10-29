#include "buildinfo.h"
#include "bridge/channel.h"
#include "filelog.h"
#include "heaptuning.h"
#include "hostoutput.h"
#include "itemschemehandler.h"
#include "networkgate.h"
#include "shellwindows.h"
#include "surfacecontroller.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QtQml/qqmlextensionplugin.h>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <cerrno>
#include <clocale>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

Q_IMPORT_QML_PLUGIN(Murasaki_RenderPlugin)

namespace {

int g_signalPipe[2] = {-1, -1};

// Async-signal-safe: only write(2) to the self-pipe; the Qt side does the real work.
void onTerminateSignal(int)
{
    const char byte = 1;
    const int saved = errno;
    [[maybe_unused]] const ssize_t written = ::write(g_signalPipe[1], &byte, 1);
    errno = saved;
}

// So SIGTERM (systemd stop) and SIGINT (Ctrl-C) unwind normally and run the destructors that unload the KWin script.
bool installQuitSignals()
{
    if (::pipe2(g_signalPipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return false;
    struct sigaction action {};
    action.sa_handler = onTerminateSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return ::sigaction(SIGTERM, &action, nullptr) == 0 && ::sigaction(SIGINT, &action, nullptr) == 0;
}

} // namespace

int main(int argc, char *argv[])
{
    murasaki::tuneHeap(); // before any thread exists
    murasaki::raiseDescriptorLimit();
    // All three must happen before QGuiApplication exists.
    murasaki::ItemSchemeHandler::registerScheme();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL); // VideoItem renders through libmpv's OpenGL API
    QtWebEngineQuick::initialize();
    // The host mode shares its OpenGL context with Qt's global one (WebEngine and mpv render there).
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QGuiApplication app(argc, argv);

    // The web engine sends all its traffic through the network gate (shut unless a shown wallpaper has the network
    // permission); this must happen before the first web profile exists.
    murasaki::applyGateEngineFlags(*murasaki::NetworkGate::instance());
    std::setlocale(LC_NUMERIC, "C"); // libmpv refuses to start with any other numeric locale
    QGuiApplication::setApplicationName(u"murasaki-surface"_s);
    QGuiApplication::setApplicationVersion(murasaki::versionString());
    QGuiApplication::setDesktopFileName(QString::fromLatin1(murasaki::kShellSurfaceAppId)); // (the GNOME extension trusts wallpaper windows by this id)

    if (installQuitSignals()) {
        auto *notifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
            char buffer[16];
            while (::read(g_signalPipe[0], buffer, sizeof buffer) > 0) {
            }
            QCoreApplication::quit();
        });
    } else {
        qWarning("murasaki-surface: cannot install signal handlers; SIGTERM will not unload the KWin script");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Murasaki wallpaper surface: draws the assigned wallpaper behind all windows."_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption windowedOption(u"windowed"_s,
                                            u"Use one normal window per screen instead of the desktop layer "
                                            "(for development)."_s);
    parser.addOption(windowedOption);
    const QCommandLineOption hostOption(u"host"_s,
                                        u"Plasma host mode: render the wallpaper of <output> offscreen and send the "
                                        "frames over the Murasaki bridge (needs --murasaki-bridge-fd)."_s,
                                        u"output"_s);
    const QCommandLineOption fdOption(u"murasaki-bridge-fd"_s,
                                      u"Host mode: the bridge socket inherited from the consumer."_s, u"fd"_s);
    const QCommandLineOption sizeOption(u"murasaki-size"_s, u"Host mode: initial render size."_s, u"WxH"_s);
    parser.addOptions({hostOption, fdOption, sizeOption});
    parser.process(app);
    if (!parser.isSet(hostOption)) // a host's stderr belongs to its consumer, which writes it to the log
        murasaki::installFileLogging(u"surface"_s);
    const bool windowed = parser.isSet(windowedOption);

    if (parser.isSet(hostOption)) {
        bool fdOk = false;
        const int fd = parser.value(fdOption).toInt(&fdOk);
        if (!fdOk || fd < 3 || ::fcntl(fd, F_GETFD) < 0) {
            qCritical("murasaki-surface: --host needs --murasaki-bridge-fd with an open descriptor");
            return 2;
        }
        // Chromium's helper processes must not inherit the socket.
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
        QSize size(1280, 720);
        if (parser.isSet(sizeOption)) {
            const QStringList parts = parser.value(sizeOption).split(u'x');
            bool wOk = false, hOk = false;
            const int width = parts.value(0).toInt(&wOk);
            const int height = parts.value(1).toInt(&hOk);
            if (parts.size() != 2 || !wOk || !hOk || width < 1 || height < 1 || width > 16384 || height > 16384) {
                qCritical("murasaki-surface: --murasaki-size must be WxH");
                return 2;
            }
            size = QSize(width, height);
        }
        murasaki::HostOutput *host = nullptr;
        murasaki::SurfaceOptions options;
        options.hostOutput = parser.value(hostOption);
        options.sinkFactory = [&host, fd, size](const murasaki::SinkRequest &request) -> std::unique_ptr<murasaki::OutputSink> {
            auto output = murasaki::HostOutput::create(request, murasaki::bridge::UniqueFd(fd), size);
            host = output.get();
            return output;
        };
        murasaki::SurfaceController controller(std::move(options));
        if (!controller.start())
            return 1;
        QObject::connect(host, &murasaki::HostOutput::pauseRequested, &controller,
                         &murasaki::SurfaceController::setExternalPause);
        QObject::connect(host, &murasaki::HostOutput::quitRequested, &app, &QCoreApplication::quit);
        // The plugin shows its failure note when the host exits non-zero.
        QObject::connect(&controller, &murasaki::SurfaceController::incompatibleDaemon, &app,
                         [&app] { app.exit(3); });
        return app.exec();
    }

#ifndef MURASAKI_HAVE_LAYERSHELL
    if (!windowed && QGuiApplication::platformName().startsWith(u"wayland"_s)) {
        qCritical("murasaki-surface was built without LayerShellQt; only --windowed is available");
        return 1;
    }
#endif
#ifdef MURASAKI_HAVE_X11
    const bool x11Session = QGuiApplication::platformName().startsWith(u"xcb"_s);
#else
    const bool x11Session = false;
#endif
    if (!windowed && !x11Session && !QGuiApplication::platformName().startsWith(u"wayland"_s)) {
        qCritical("murasaki-surface needs a Wayland session with layer-shell or an X11 session (found \"%s\"); use --windowed",
                  qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    murasaki::SurfaceOptions options;
    options.windowed = windowed;
    murasaki::SurfaceController controller(std::move(options));
    if (!controller.start())
        return 1;
    return app.exec();
}
