#include "bridgeitem.h"

#include "bridgeclient.h"
#include "bridgenode.h"
#include "dmabufimport.h"
#include "filelog.h"

#include <QFileInfo>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QProcess>
#include <QQuickOpenGLUtils>
#include <QRunnable>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QtQuick/qsgtexture_platform.h>

#include <sys/prctl.h>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <vector>

using namespace Qt::StringLiterals;

namespace murasaki {

using namespace bridge;

namespace {

constexpr int kResizeDebounceMs = 200;
constexpr int kReapMs = 2000;
constexpr int kLogLines = 20;

// Waits for a stopped host process to exit and kills it after kReapMs. It owns the QProcess, drains its output so the
// host never blocks on a full pipe, and deletes itself when the process is gone.
class HostReaper : public QObject
{
public:
    explicit HostReaper(QProcess *process)
        : m_process(process)
    {
        process->setParent(this);
        connect(process, &QProcess::readyRead, this, [process] { process->readAll(); });
        connect(process, &QProcess::finished, this, &QObject::deleteLater);
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
                deleteLater();
        });
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [process] { process->kill(); });
        timer->start(kReapMs);
        if (process->state() == QProcess::NotRunning)
            deleteLater();
    }

private:
    QProcess *m_process;
};

} // namespace

BridgeItem::BridgeItem(QQuickItem *parent)
    : QQuickItem(parent)
    , m_releases(std::make_shared<BridgeReleaseState>())
{
    setFlag(ItemHasContents);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    m_clock.start();
    m_restartTimer.setSingleShot(true);
    connect(&m_restartTimer, &QTimer::timeout, this, &BridgeItem::startHost);
    m_exitTimer.setSingleShot(true);
    connect(&m_exitTimer, &QTimer::timeout, this, [this] {
        if (m_process && m_process->state() != QProcess::NotRunning)
            m_process->kill(); // (finished follows, with CrashExit)
    });
    connect(&m_programWatch, &ProgramWatch::changed, this, [this](const QString &path) {
        if (!m_completed)
            return;
        qInfo("BridgeItem: %s was replaced on disk; restarting %s", qPrintable(path), qPrintable(m_hostName));
        reloadHost();
    });
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(kResizeDebounceMs);
    m_parkTimer.setSingleShot(true);
    connect(&m_parkTimer, &QTimer::timeout, this, &BridgeItem::park);
    connect(&m_resizeTimer, &QTimer::timeout, this, &BridgeItem::sendResize);
}

BridgeItem::~BridgeItem()
{
    stopHost();
}

// ---- properties ----------------------------------------------------------------------------------------

void BridgeItem::setProgram(const QString &program)
{
    if (program == m_program)
        return;
    m_program = program;
    emit programChanged();
    if (m_completed)
        restartHost();
}

void BridgeItem::setArguments(const QStringList &arguments)
{
    if (arguments == m_arguments)
        return;
    m_arguments = arguments;
    emit argumentsChanged();
    if (m_completed)
        restartHost();
}

bool BridgeItem::handleHostLost(const QString &)
{
    return false;
}

bool BridgeItem::canStart() const
{
    return !m_program.isEmpty();
}

QString BridgeItem::logName(const BridgeLaunch &launch) const
{
    QString name = QFileInfo(launch.program).fileName();
    const qsizetype host = launch.arguments.indexOf(u"--host"_s);
    if (host >= 0 && host + 1 < launch.arguments.size())
        name += u'-' + launch.arguments.at(host + 1);
    return name;
}

BridgeLaunch BridgeItem::buildLaunch(int bridgeFd, const QSize &size) const
{
    BridgeLaunch launch;
    launch.program = m_program;
    launch.arguments = m_arguments;
    launch.arguments << u"--murasaki-bridge-fd"_s << QString::number(bridgeFd) << u"--murasaki-size"_s
                     << u"%1x%2"_s.arg(size.width()).arg(size.height());
    return launch;
}

void BridgeItem::setProperties(const QVariantMap &properties)
{
    if (properties == m_properties)
        return;
    m_properties = properties;
    emit propertiesChanged();
    propertiesApplied(m_properties);
    syncLive();
}

void BridgeItem::setFill(const QString &fill)
{
    if (fill == m_fill)
        return;
    m_fill = fill;
    emit fillChanged();
    syncLive();
}

void BridgeItem::setSpan(const QVariantMap &span)
{
    if (span == m_span)
        return;
    m_span = span;
    emit spanChanged();
    syncLive();
}

void BridgeItem::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    emit pausedChanged();
    if (paused && m_parkAfterMs > 0) {
        m_parkTimer.start(m_parkAfterMs);
    } else if (!paused) {
        m_parkTimer.stop();
        if (m_parked) {
            m_parked = false;
            qInfo("BridgeItem: the pause ended; starting %s again", qPrintable(m_hostName));
            scheduleStart(); // (the last frame stays until the new host draws)
            return;
        }
    }
    syncLive();
}

void BridgeItem::setParkAfterMs(int ms)
{
    m_parkAfterMs = ms;
    if (m_paused && ms > 0 && !m_parked)
        m_parkTimer.start(ms);
    else if (ms <= 0)
        m_parkTimer.stop();
}

void BridgeItem::park()
{
    // (a spanned picture is not: it would start over on this output only, out of step with the others)
    if (!m_paused || m_parked || !m_process || spanEnabled())
        return;
    qInfo("BridgeItem: paused for %d s; stopping %s until the pause ends", m_parkAfterMs / 1000, qPrintable(m_hostName));
    m_parked = true;
    stopHost();
    m_restartTimer.stop();
    update(); // keep the last frame on screen
}

void BridgeItem::setAudible(bool audible)
{
    if (audible == m_audible)
        return;
    m_audible = audible;
    emit audibleChanged();
    syncLive();
}

void BridgeItem::setVolume(qreal volume)
{
    volume = std::clamp(volume, qreal(0), qreal(1));
    if (qFuzzyCompare(volume + 1, m_volume + 1))
        return;
    m_volume = volume;
    emit volumeChanged();
    syncLive();
}

void BridgeItem::setFps(int fps)
{
    fps = std::max(1, fps);
    if (fps == m_fps)
        return;
    m_fps = fps;
    emit fpsChanged();
    syncLive();
}

uint32_t BridgeItem::effectiveVolume() const
{
    return m_audible ? static_cast<uint32_t>(std::lround(m_volume * 128.0)) : 0;
}

QSize BridgeItem::pixelSize() const
{
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize size(static_cast<int>(std::lround(width() * dpr)), static_cast<int>(std::lround(height() * dpr)));
    return size.width() > 0 && size.height() > 0
        ? QSize(std::min<int>(size.width(), kMaxDimension), std::min<int>(size.height(), kMaxDimension))
        : QSize();
}

// ---- life cycle ----------------------------------------------------------------------------------------

void BridgeItem::componentComplete()
{
    QQuickItem::componentComplete();
    m_completed = true;
    connectWindow(window());
    scheduleStart();
}

void BridgeItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        connectWindow(value.window);
        scheduleStart();
    } else if (change == ItemDevicePixelRatioHasChanged) {
        m_resizeTimer.start();
        scheduleStart();
    }
}

void BridgeItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        m_resizeTimer.start();
        scheduleStart();
    }
}

void BridgeItem::connectWindow(QQuickWindow *window)
{
    disconnect(m_swapConnection);
    m_swapConnection = {};
    if (!window)
        return;
    // Direct: the signal may come from the render thread, and the state it touches is mutex protected.
    m_swapConnection = connect(window, &QQuickWindow::frameSwapped, this,
                               [releases = m_releases] { releases->flush(); }, Qt::DirectConnection);
    // The graphics API is only known once the scene graph has initialised.
    connect(window, &QQuickWindow::sceneGraphInitialized, this, &BridgeItem::scheduleStart, Qt::UniqueConnection);
}

bool BridgeItem::openGlBackend(bool *known) const
{
    QSGRendererInterface *ri = window() ? window()->rendererInterface() : nullptr;
    const auto api = ri ? ri->graphicsApi() : QSGRendererInterface::Unknown;
    *known = api != QSGRendererInterface::Unknown;
    return api == QSGRendererInterface::OpenGL;
}

void BridgeItem::scheduleStart()
{
    if (m_startQueued)
        return;
    m_startQueued = true;
    // Bindings set several properties one after another; start once they have all landed.
    QMetaObject::invokeMethod(this, [this] {
        m_startQueued = false;
        startHost();
    }, Qt::QueuedConnection);
}

void BridgeItem::setHasFrame(bool has)
{
    if (m_hasFrame == has)
        return;
    m_hasFrame = has;
    // The nested host test checks this line: it proves the engine (not the preview placeholder) drew.
    if (has && (qEnvironmentVariableIsSet("MURASAKI_BRIDGE_LOG") || qEnvironmentVariableIsSet("MURASAKI_SCENE_LOG")))
        qInfo("murasaki.bridge: hasFrame (first frame from the host process)");
    emit hasFrameChanged();
}

void BridgeItem::restartHost()
{
    // A new configuration shows its own first frame; until then the old one may still be on screen. (A crash
    // restart keeps the flag: the last frame stays visible.)
    m_frameReported = false;
    setHasFrame(false);
    stopHost();
    m_restartTimer.stop();
    m_backoff.reset();
    m_gaveUp = false;
    scheduleStart();
}

void BridgeItem::reloadHost()
{
    // (hasFrame and the frame on screen stay until the new host draws; a host that gave up is given another chance with the new file, and one that crashes goes through the
    // usual backoff again)
    stopHost();
    m_restartTimer.stop();
    m_backoff.reset();
    m_gaveUp = false;
    scheduleStart();
}

void BridgeItem::wireProcess(QProcess *process, const QString &logName)
{
    connect(process, &QProcess::readyRead, this, [this, process, logName] {
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine()).trimmed();
            if (line.isEmpty())
                continue;
            m_log.append(line);
            appendChildLog(logName, line); // plasmashell's pipe is not a place to lose an engine's last words
            while (m_log.size() > kLogLines)
                m_log.removeFirst();
            if (qEnvironmentVariableIsSet("MURASAKI_BRIDGE_LOG") || qEnvironmentVariableIsSet("MURASAKI_SCENE_LOG"))
                qInfo("%s: %s", qPrintable(m_hostName), qPrintable(line));
        }
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        hostExited(code, status);
        QString reason = status == QProcess::CrashExit ? m_hostName + u" crashed"_s
                                                       : m_hostName + u" exited with code %1"_s.arg(code);
        const QString last = m_client ? m_client->lastEngineError() : QString();
        if (!last.isEmpty())
            reason += u": "_s + last;
        else if (!m_log.isEmpty())
            reason += u": "_s + m_log.last();
        onHostLost(reason);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onHostLost(m_hostName + u" failed to start"_s);
    });
}

void BridgeItem::adoptHost(QProcess *process)
{
    wireProcess(process, QStringLiteral("adopted"));
    m_process = process;
}

void BridgeItem::startHost()
{
    if (m_parked)
        return; // the end of the pause starts it
    if (m_process || m_gaveUp || !m_completed || !canStart() || !window())
        return;
    if (m_exitWaitMs > 0 && HostRetirement::anyLeaving()) {
        // the engine that was stopped is still on its way out: the next one waits for it (at most one alive)
        if (!m_restartTimer.isActive())
            m_restartTimer.start(25);
        return;
    }
    bool known = false;
    const bool opengl = openGlBackend(&known);
    if (!known)
        return; // sceneGraphInitialized calls back
    if (!opengl) {
        m_gaveUp = true;
        emit failed(u"unsupported graphics backend"_s);
        return;
    }
    if (const QString problem = configurationError(); !problem.isEmpty()) {
        m_gaveUp = true;
        emit failed(problem);
        return;
    }
    const QSize size = pixelSize();
    if (size.isEmpty())
        return;
    auto pair = makeSocketPair();
    if (!pair) {
        m_gaveUp = true;
        emit failed(m_noun.isEmpty() ? u"cannot create the bridge socket"_s
                                     : u"cannot create the %1 bridge socket"_s.arg(m_noun));
        return;
    }
    const int childFd = pair->second.get();
    const BridgeLaunch launch = buildLaunch(childFd, size);
    if (!launch.error.isEmpty() || launch.program.isEmpty()) {
        m_gaveUp = true;
        emit failed(launch.error.isEmpty() ? u"no program to launch"_s : launch.error);
        return;
    }

    m_applied = {};
    m_applied.size = launch.size.isEmpty() ? size : launch.size;
    m_applied.paused = false; // the host starts running; syncLive() pauses it after Hello
    if (launch.carriesState) {
        m_applied.fill = m_fill;
        m_applied.fps = m_fps;
        m_applied.volume = effectiveVolume();
        m_applied.properties = propertiesForEngine(m_properties);
    } else {
        // Nothing was passed on the command line: everything is sent as live state once Hello arrives.
        m_applied.volume = std::numeric_limits<uint32_t>::max();
    }

    m_log.clear();
    m_programWatch.watch(launch.program); // the file this host runs now; replaced later, the host is started again
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProgram(launch.program);
    process->setArguments(launch.arguments);
    const pid_t parentPid = ::getpid();
    process->setChildProcessModifier([childFd, parentPid] {
        // Only async-signal-safe calls here: this runs in the forked child. The host end of the socketpair is the
        // one descriptor that must survive exec; our own end stays close-on-exec.
        const int flags = ::fcntl(childFd, F_GETFD);
        if (flags >= 0)
            ::fcntl(childFd, F_SETFD, flags & ~FD_CLOEXEC);
        // Never outlive a crashed surface. SIGKILL: nothing in the host may delay it. The surface could have died
        // between fork() and prctl(), in which case no signal would ever come.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parentPid)
            ::_exit(1);
    });
    wireProcess(process, this->logName(launch));
    m_process = process;

    m_client = new BridgeClient(std::move(pair->first), this);
    m_client->setPeerName(m_hostName);
    if (!m_noun.isEmpty())
        m_client->setSocketName(m_noun + u" bridge socket"_s);
    m_queue = m_client->frames();
    connect(m_client, &BridgeClient::ready, this, &BridgeItem::onReady);
    connect(m_client, &BridgeClient::frameAvailable, this, &QQuickItem::update);
    connect(m_client, &BridgeClient::lost, this, &BridgeItem::onHostLost);

    process->start();
    // start() has forked by now (or failed, reported through errorOccurred): drop our copy of the host's end.
    pair->second.reset();
}

void BridgeItem::stopHost()
{
    m_exitTimer.stop(); // (it watches the process being stopped; it must not kill the next one)
    m_resizeTimer.stop();
    if (m_client) {
        m_client->disconnect(this);
        m_client->sendQuit();
        m_client->close(); // EOF is the host's second cue to quit
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_process) {
        m_process->disconnect(this);
        if (m_exitWaitMs > 0) {
            // Never two hosts alive at once, and never waiting for one: it is retired (asked to end, killed after a moment) and the next host is not started until it is gone (startHost).
            // (a crash of its own while it ends is still the model's: it is told to retiredHostExited)
            QPointer<BridgeItem> self(this);
            HostRetirement::retire(m_process, m_exitWaitMs, [self](int code, QProcess::ExitStatus status) {
                if (self)
                    self->retiredHostExited(code, status);
            });
        } else {
            new HostReaper(m_process);
        }
        m_process = nullptr;
    }
}

void BridgeItem::onHostLost(const QString &reason)
{
    if (!m_process && !m_client)
        return;
    if (m_process && m_process->state() != QProcess::NotRunning && m_exitWaitMs > 0) {
        // (a host that crashed has closed its socket before we heard it exit: its status is what tells a refusal from a crash.) Not waited for: the `finished` handler comes back here with the status, and a host that
        // closed its socket and lives on is broken - it is killed after a moment, which is a crash.
        if (!m_exitTimer.isActive())
            m_exitTimer.start(m_exitWaitMs);
        return;
    }
    m_exitTimer.stop();
    stopHost();
    update(); // keep the last frame on screen until the restart produces a new one
    if (handleHostLost(reason)) {
        qWarning("BridgeItem: %s; restarting with another configuration", qPrintable(reason));
        m_backoff = RestartBackoff();
        m_restartTimer.start(0);
        return;
    }
    const auto delay = m_backoff.recordCrash(std::chrono::milliseconds(m_clock.elapsed()));
    if (!delay) {
        m_gaveUp = true;
        emit failed(reason);
        return;
    }
    qWarning("BridgeItem: %s; restarting in %lld ms", qPrintable(reason), static_cast<long long>(delay->count()));
    m_restartTimer.start(static_cast<int>(delay->count()));
}

// ---- live state ----------------------------------------------------------------------------------------

void BridgeItem::onRenderContextLost()
{
    if (m_gaveUp)
        return;
    m_gaveUp = true;
    stopHost();
    m_restartTimer.stop();
    emit failed(m_noun.isEmpty() ? u"rendering context unavailable"_s
                                 : m_noun + u" rendering context unavailable"_s);
}

void BridgeItem::onReady()
{
    syncLive();
    if (m_client)
        clientReady(m_client);
}

void BridgeItem::syncLive()
{
    if (!m_client || !m_client->isReady())
        return;
    if (m_paused != m_applied.paused) {
        m_applied.paused = m_paused;
        m_client->sendPause(m_paused);
    }
    if (m_fill != m_applied.fill) {
        m_applied.fill = m_fill;
        m_client->sendScaling(bridgeScalingMode(m_fill));
    }
    if (m_span != m_applied.span && m_span != m_refusedSpan) {
        const auto number = [&](const char *key) { return m_span.value(QLatin1String(key)).toDouble(); };
        const bool sent = m_client->sendSpanView(!m_span.isEmpty(), number("canvasW"), number("canvasH"), number("x"),
                                                 number("y"), number("w"), number("h"));
        if (sent) {
            m_applied.span = m_span;
            m_refusedSpan.clear();
        } else {
            m_refusedSpan = m_span; // said once, not at every later sync
            const QString what = m_noun.isEmpty() ? u"wallpaper"_s : m_noun;
            // A host that knows SpanView got a rectangle it cannot use; an older one does not know it at all.
            emit failed(m_client->supportsSpan() ? u"invalid span geometry for the %1 across outputs"_s.arg(what)
                                                 : u"%1 cannot draw a %2 across outputs"_s.arg(m_hostName, what));
        }
    }
    if (m_fps != m_applied.fps) {
        m_applied.fps = m_fps;
        m_client->sendFps(static_cast<uint32_t>(m_fps));
    }
    const uint32_t volume = effectiveVolume();
    if (volume != m_applied.volume) {
        m_applied.volume = volume;
        m_client->sendVolume(volume);
    }
    const QVariantMap wanted = propertiesForEngine(m_properties);
    if (wanted != m_applied.properties) {
        QStringList changed;
        for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
            const auto old = m_applied.properties.constFind(it.key());
            if (old == m_applied.properties.cend() || *old != it.value())
                changed << it.key();
        }
        m_applied.properties = wanted;
        if (!changed.isEmpty())
            m_client->sendProperties(bridgePropertiesJson(wanted, changed));
    }
}

void BridgeItem::sendResize()
{
    if (!m_process) {
        scheduleStart();
        return;
    }
    const QSize size = bufferSize(pixelSize());
    // Before Hello the client remembers the size and sends it once the viewport is announced.
    if (size.isEmpty() || size == m_applied.size || !m_client)
        return;
    m_applied.size = size;
    m_client->sendResize(static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()));
}

// ---- pointer -------------------------------------------------------------------------------------------

void BridgeItem::forwardPointer(const QPointF &position, Qt::MouseButtons buttons)
{
    if (!m_client || !m_client->isReady())
        return;
    if (qEnvironmentVariableIsSet("MURASAKI_BRIDGE_LOG") || qEnvironmentVariableIsSet("MURASAKI_SCENE_LOG")) {
        static int forwarded = 0; // debugging aid: shows that pointer input reaches the scene
        if (forwarded++ % 50 == 0)
            qInfo("BridgeItem: pointer events forwarded: %d", forwarded);
    }
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    uint32_t mask = 0;
    if (buttons & Qt::LeftButton)
        mask |= 1;
    if (buttons & Qt::RightButton)
        mask |= 2;
    if (buttons & Qt::MiddleButton)
        mask |= 4;
    // a button went down: the host itself saw a real press (a scene's script cannot make one), which shortcuts need before they start anything
    if ((mask & ~m_lastButtons) != 0)
        emit buttonPressed();
    m_lastButtons = mask;
    m_client->sendPointer(static_cast<int>(std::lround(position.x() * dpr)),
                          static_cast<int>(std::lround(position.y() * dpr)), mask);
}

void BridgeItem::hoverEnterEvent(QHoverEvent *event)
{
    forwardPointer(event->position(), Qt::NoButton);
}

void BridgeItem::hoverMoveEvent(QHoverEvent *event)
{
    forwardPointer(event->position(), Qt::NoButton);
}

void BridgeItem::mousePressEvent(QMouseEvent *event)
{
    forwardPointer(event->position(), event->buttons());
    event->accept();
}

void BridgeItem::mouseMoveEvent(QMouseEvent *event)
{
    forwardPointer(event->position(), event->buttons());
}

void BridgeItem::mouseReleaseEvent(QMouseEvent *event)
{
    forwardPointer(event->position(), event->buttons());
}

// ---- rendering -----------------------------------------------------------------------------------------

QSGNode *BridgeItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<BridgeNode *>(oldNode);
    if (!node)
    {
        QPointer<BridgeItem> self(this);
        node = new BridgeNode(window(), m_releases, [self] {
            // Render thread: hop to the GUI thread, which owns the item's state.
            if (self)
                QMetaObject::invokeMethod(self.data(), [self] {
                    if (self)
                        self->onRenderContextLost();
                }, Qt::QueuedConnection);
        });
    }
    if (node->update(m_queue, boundingRect()) && !m_frameReported.exchange(true)) {
        QPointer<BridgeItem> self(this);
        QMetaObject::invokeMethod(this, [self] {
            if (self)
                self->setHasFrame(true);
        }, Qt::QueuedConnection);
    }
    if (!node->hasTexture()) {
        delete node; // nothing to show yet; only a node created in this call can be texture-less
        return nullptr;
    }
    return node;
}

} // namespace murasaki
