#include "kwinmonitor.h"

#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <memory>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

const QString kScriptName = u"murasaki-monitor"_s;
const QString kCursorScriptName = u"murasaki-cursor"_s;

constexpr int kCallTimeoutMs = 1000; // KWin may be wedged or gone; never block a start or a shutdown for 25 s
constexpr int kMaxLoadRetries = 3;
constexpr int kRetryDelayMs = 2000;

std::unique_ptr<QDBusInterface> scripting()
{
    auto iface = std::make_unique<QDBusInterface>(u"org.kde.KWin"_s, u"/Scripting"_s, u"org.kde.kwin.Scripting"_s);
    iface->setTimeout(kCallTimeoutMs);
    return iface;
}

} // namespace

KWinMonitor::KWinMonitor(QObject *parent)
    : SessionMonitor(parent)
{
    // KWin reports every window event; one state update per burst is enough.
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(100);
    connect(&m_coalesce, &QTimer::timeout, this, &KWinMonitor::applyPending);
}

KWinMonitor::~KWinMonitor()
{
    unloadCursorScript();
    if (m_loaded)
        scripting()->call(u"unloadScript"_s, kScriptName);
}

void KWinMonitor::start()
{
    // KWin loads scripts from a file path, so the bundled script is written to the runtime dir first.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + u"/murasaki"_s;
    m_scriptPath = dir + u"/kwin-monitor.js"_s;
    QFile source(u":/murasaki/kwin-monitor.js"_s);
    QSaveFile target(m_scriptPath);
    if (!QDir().mkpath(dir) || !source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)
        // The surface's own layer-shell windows look like normal full-output windows to KWin; the script skips
        // them by pid, so they must not pause themselves.
        || target.write("const surfacePid = " + QByteArray::number(QCoreApplication::applicationPid()) + ";\n") < 0
        || target.write(source.readAll()) < 0 || !target.commit()) {
        qWarning("murasaki-surface: cannot write %s; wallpapers will not pause or follow desktops",
                 qPrintable(m_scriptPath));
        return;
    }
    m_cursorScriptPath = dir + u"/kwin-cursor.js"_s;
    QFile cursorSource(u":/murasaki/kwin-cursor.js"_s);
    QSaveFile cursorTarget(m_cursorScriptPath);
    if (!cursorSource.open(QIODevice::ReadOnly) || !cursorTarget.open(QIODevice::WriteOnly)
        || cursorTarget.write(cursorSource.readAll()) < 0 || !cursorTarget.commit()) {
        qWarning("murasaki-surface: cannot write %s; Plasma wallpapers will not follow the mouse",
                 qPrintable(m_cursorScriptPath));
        m_cursorScriptPath.clear();
    }
    // A restarted or replaced KWin forgets the script; load it again when the name reappears.
    m_watcher = new QDBusServiceWatcher(u"org.kde.KWin"_s, QDBusConnection::sessionBus(),
                                        QDBusServiceWatcher::WatchForRegistration, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
        m_retries = 0;
        m_cursorLoaded = false; // the new KWin has no scripts
        loadScript();
        if (m_cursorWanted)
            loadCursorScript();
    });
    loadScript();
    if (m_cursorWanted)
        loadCursorScript();
}

void KWinMonitor::setCursorReporting(bool enabled)
{
    if (enabled == m_cursorWanted)
        return;
    m_cursorWanted = enabled;
    if (m_scriptPath.isEmpty()) // start() has not run (or failed): it loads the script itself if still wanted
        return;
    if (enabled)
        loadCursorScript();
    else
        unloadCursorScript();
}

void KWinMonitor::loadCursorScript()
{
    if (m_cursorScriptPath.isEmpty() || m_cursorLoaded)
        return;
    const auto kwin = scripting();
    if (QDBusReply<bool>(kwin->call(u"isScriptLoaded"_s, kCursorScriptName)).value())
        kwin->call(u"unloadScript"_s, kCursorScriptName); // left behind by a crashed surface
    const QDBusReply<int> id = kwin->call(u"loadScript"_s, m_cursorScriptPath, kCursorScriptName);
    if (!id.isValid() || id.value() < 0) {
        qWarning("murasaki-surface: KWin refused the cursor script (%s); Plasma wallpapers will not follow the mouse",
                 qPrintable(id.error().message()));
        return;
    }
    const QDBusMessage started = kwin->call(u"start"_s);
    if (started.type() == QDBusMessage::ErrorMessage) {
        qWarning("murasaki-surface: KWin could not start the cursor script (%s)", qPrintable(started.errorMessage()));
        return;
    }
    m_cursorLoaded = true;
}

void KWinMonitor::unloadCursorScript()
{
    if (!m_cursorLoaded)
        return;
    m_cursorLoaded = false;
    scripting()->call(u"unloadScript"_s, kCursorScriptName);
}

void KWinMonitor::loadScript()
{
    const auto kwin = scripting();
    if (QDBusReply<bool>(kwin->call(u"isScriptLoaded"_s, kScriptName)).value())
        kwin->call(u"unloadScript"_s, kScriptName); // left behind by a crashed surface
    const QDBusReply<int> id = kwin->call(u"loadScript"_s, m_scriptPath, kScriptName);
    if (!id.isValid() || id.value() < 0) {
        if (m_retries < kMaxLoadRetries) {
            ++m_retries;
            QTimer::singleShot(kRetryDelayMs, this, &KWinMonitor::loadScript);
            return;
        }
        qWarning("murasaki-surface: KWin refused the monitor script (%s); wallpapers will not pause or follow "
                 "desktops", qPrintable(id.error().message()));
        return;
    }
    m_retries = 0;
    const QDBusMessage started = kwin->call(u"start"_s);
    if (started.type() == QDBusMessage::ErrorMessage) {
        qWarning("murasaki-surface: KWin could not start the monitor script (%s); wallpapers will not pause or "
                 "follow desktops", qPrintable(started.errorMessage()));
        return;
    }
    m_loaded = true;
}

void KWinMonitor::report(const QString &json)
{
    m_pending = json;
    if (!m_coalesce.isActive()) // restarting would starve updates under a steady stream
        m_coalesce.start();
}

void KWinMonitor::applyPending()
{
    const QJsonDocument doc = QJsonDocument::fromJson(m_pending.toUtf8());
    if (!doc.isObject()) {
        qWarning("murasaki-surface: ignoring a malformed KWin report");
        return;
    }
    setState(parseKWinReport(doc.object()));
}

} // namespace murasaki
