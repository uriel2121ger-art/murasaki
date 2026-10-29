#include "kwinpointersource.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

const QString kScriptName = u"murasaki-pet-cursor"_s;

QDBusMessage scriptingCall(const KWinPointerSource::Options &o, const QString &method, const QVariantList &args = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(o.kwinService, u"/Scripting"_s, u"org.kde.kwin.Scripting"_s, method);
    message.setArguments(args);
    return message;
}

} // namespace

KWinPointerSource::KWinPointerSource(QObject *parent)
    : KWinPointerSource(Options(), parent)
{
}

KWinPointerSource::KWinPointerSource(const Options &options, QObject *parent)
    : PointerSource(parent)
    , m_options(options)
    , m_scriptName(kScriptName)
{
    if (m_options.runtimeDir.isEmpty())
        m_options.runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + u"/murasaki"_s;
    m_clock.start();
    m_trailing.setSingleShot(true);
    m_trailing.setInterval(kMinIntervalMs);
    connect(&m_trailing, &QTimer::timeout, this, &KWinPointerSource::flushPending);
    m_registered = m_options.bus.registerObject(QString::fromLatin1(kObjectPath), this, QDBusConnection::ExportScriptableSlots);
    if (!m_registered)
        qWarning("murasaki-overlay: cannot export %s on the session bus; the KWin cursor source will not work", kObjectPath);
    // Who owns org.kde.KWin, kept here so that a report is judged without a bus round trip, and a restarted KWin (no scripts, a new owner)
    // gets the script again while it is wanted.
    m_watcher = new QDBusServiceWatcher(m_options.kwinService, m_options.bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &, const QString &newOwner) {
        setKWinOwner(newOwner);
        m_loaded = false; // the new KWin has no scripts (and the old one took ours with it)
        m_mayExist = false;
        m_retried = false;
        if (!newOwner.isEmpty() && m_wanted)
            load();
    });
    if (const QDBusConnectionInterface *interface = m_options.bus.interface()) {
        const QDBusReply<QString> owner = interface->serviceOwner(m_options.kwinService);
        if (owner.isValid())
            m_kwinOwner = owner.value();
    }
    // A script left by an overlay that was killed would go on reporting the cursor to whoever owns our name: take it out now, whether or
    // not anything wants the cursor. (Sent first, so it is ordered before any load that follows on this connection.)
    if (!m_kwinOwner.isEmpty())
        m_options.bus.asyncCall(scriptingCall(m_options, u"unloadScript"_s, {m_scriptName}), m_options.callTimeoutMs);
}

KWinPointerSource::~KWinPointerSource()
{
    m_wanted = false;
    ++m_generation;
    if (m_registered)
        m_options.bus.unregisterObject(QString::fromLatin1(kObjectPath)); // no report reaches a half destroyed object
    sendUnload(true);
}

void KWinPointerSource::setKWinOwner(const QString &owner)
{
    m_kwinOwner = owner;
}

bool KWinPointerSource::calledByKWin() const
{
    if (!calledFromDBus())
        return true; // a direct call (tests, the in-process owner) is trusted
    return !m_kwinOwner.isEmpty() && message().service() == m_kwinOwner;
}

bool KWinPointerSource::validServiceName(const QString &name)
{
    static const QRegularExpression shape(u"^[A-Za-z0-9._-]{1,255}$"_s);
    return shape.match(name).hasMatch();
}

void KWinPointerSource::ReportPointer(int x, int y)
{
    if (!m_wanted || x < -kMaxCoordinate || x > kMaxCoordinate || y < -kMaxCoordinate || y > kMaxCoordinate || !calledByKWin())
        return;
    const QPointF p(x, y);
    const qint64 now = m_clock.elapsed();
    if (now - m_lastEmitMs >= kMinIntervalMs && !m_trailing.isActive()) {
        emitMoved(p);
        return;
    }
    m_pending = p; // coalesced: only the latest goes out, when the interval ends
    if (!m_trailing.isActive())
        m_trailing.start(int(std::max<qint64>(1, kMinIntervalMs - (now - m_lastEmitMs))));
}

void KWinPointerSource::flushPending()
{
    if (!m_pending || !m_wanted) {
        m_pending.reset();
        return;
    }
    const QPointF p = *m_pending;
    m_pending.reset();
    emitMoved(p);
}

void KWinPointerSource::emitMoved(QPointF p)
{
    m_lastEmitMs = m_clock.elapsed();
    emit moved(p);
}

void KWinPointerSource::wanted(bool on)
{
    if (on == m_wanted)
        return;
    if (on && !m_registered) { // nothing could ever call us: say so instead of sitting at a global level with a dead source
        emit unavailable(u"the pointer object could not be exported on the session bus"_s);
        return;
    }
    m_wanted = on;
    ++m_generation;
    m_retried = false;
    if (on) {
        load();
    } else {
        m_pending.reset();
        m_trailing.stop();
        sendUnload(false);
    }
}

// The bundled script, with the target service replaced when the overlay's name is not the default, written to the runtime directory
// (KWin loads scripts from a path).
bool KWinPointerSource::writeScript()
{
    QFile source(u":/murasaki/kwin-pet-cursor.js"_s);
    if (!source.open(QIODevice::ReadOnly))
        return false;
    QByteArray text = source.readAll();
    if (m_options.serviceName != QLatin1String(kDefaultServiceName)) {
        // Only the one `const service = "...";` line: the interface name has the service name as a prefix and must stay as it is.
        if (!validServiceName(m_options.serviceName))
            return false;
        const QByteArray literal = "const service = \"" + QByteArray(kDefaultServiceName) + "\";";
        if (!text.contains(literal))
            return false;
        text.replace(literal, "const service = \"" + m_options.serviceName.toUtf8() + "\";");
    }
    m_scriptPath = m_options.runtimeDir + u"/kwin-pet-cursor.js"_s;
    QSaveFile target(m_scriptPath);
    return QDir().mkpath(m_options.runtimeDir) && target.open(QIODevice::WriteOnly) && target.write(text) == text.size() && target.commit();
}

void KWinPointerSource::load()
{
    if (m_loaded || m_loading)
        return;
    if (!writeScript()) {
        emit unavailable(u"cannot write the KWin cursor script to "_s + m_options.runtimeDir);
        return;
    }
    m_loading = true;
    const int generation = m_generation;
    // isScriptLoaded -> (unloadScript) -> loadScript -> start, all asynchronous: KWin may be slow or gone and the overlay must not wait.
    // A failure is retried once when KWin is on the bus (it may just have restarted); the second one is reported.
    const auto fail = [this, generation](const QString &why) {
        m_loading = false;
        if (generation != m_generation) { // wanted() changed meanwhile: whatever is wanted now starts from a clean state
            if (m_wanted)
                load();
            return;
        }
        if (!m_wanted)
            return;
        if (!m_retried && !m_kwinOwner.isEmpty()) {
            m_retried = true;
            load();
            return;
        }
        emit unavailable(why);
    };
    // A step that answers after wanted() changed (or the source is being destroyed) goes no further: it takes the script out if it may be
    // there and, when something is wanted again, starts over.
    const auto abandon = [this] {
        m_options.bus.asyncCall(scriptingCall(m_options, u"unloadScript"_s, {m_scriptName}), m_options.callTimeoutMs);
        m_mayExist = false;
        m_loading = false;
        if (m_wanted)
            load();
    };
    const auto startIt = [this, generation, fail] {
        auto *started = new QDBusPendingCallWatcher(m_options.bus.asyncCall(scriptingCall(m_options, u"start"_s), m_options.callTimeoutMs), this);
        connect(started, &QDBusPendingCallWatcher::finished, this, [this, generation, fail](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            if (w->isError()) {
                sendUnload(false); // a loaded script that was never started is not left in KWin
                fail(u"KWin could not start the cursor script: "_s + w->error().message());
                return;
            }
            m_loading = false;
            if (generation != m_generation) { // wanted() changed while the script was being loaded
                sendUnload(false);
                if (m_wanted)
                    load(); // wanted(false), wanted(true) in quick succession: start again from a clean state
                return;
            }
            m_loaded = true;
            m_retried = false;
        });
    };
    const auto loadIt = [this, generation, fail, startIt, abandon] {
        if (generation != m_generation) {
            abandon();
            return;
        }
        m_mayExist = true;
        auto *loaded = new QDBusPendingCallWatcher(
            m_options.bus.asyncCall(scriptingCall(m_options, u"loadScript"_s, {m_scriptPath, m_scriptName}), m_options.callTimeoutMs), this);
        connect(loaded, &QDBusPendingCallWatcher::finished, this, [this, generation, fail, startIt, abandon](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            const QDBusPendingReply<int> reply = *w;
            if (generation != m_generation) {
                abandon();
                return;
            }
            if (reply.isError() || reply.value() < 0) {
                fail(u"KWin refused the cursor script: "_s + (reply.isError() ? reply.error().message() : QStringLiteral("negative id")));
                return;
            }
            startIt();
        });
    };
    auto *check = new QDBusPendingCallWatcher(
        m_options.bus.asyncCall(scriptingCall(m_options, u"isScriptLoaded"_s, {m_scriptName}), m_options.callTimeoutMs), this);
    connect(check, &QDBusPendingCallWatcher::finished, this, [this, generation, fail, loadIt, abandon](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<bool> reply = *w;
        if (generation != m_generation) {
            m_loading = false;
            if (m_wanted)
                load();
            return;
        }
        if (reply.isError()) {
            fail(u"KWin does not answer on its scripting interface: "_s + reply.error().message());
            return;
        }
        if (reply.value()) { // left behind by a crashed overlay
            auto *gone = new QDBusPendingCallWatcher(
                m_options.bus.asyncCall(scriptingCall(m_options, u"unloadScript"_s, {m_scriptName}), m_options.callTimeoutMs), this);
            connect(gone, &QDBusPendingCallWatcher::finished, this, [gone, loadIt] { // (loadIt re-checks the generation)
                gone->deleteLater();
                loadIt();
            });
        } else {
            loadIt();
        }
    });
}

// Takes the script out of KWin whenever it may be there: loaded, being loaded, or a loadScript sent whose answer is not in yet.
// `blocking` is the destructor's: it waits (running the event loop, so a peer in this process can answer) for a short time.
void KWinPointerSource::sendUnload(bool blocking)
{
    if (!m_loaded && !m_loading && !m_mayExist)
        return;
    m_loaded = false;
    m_mayExist = false;
    if (blocking) {
        m_options.bus.call(scriptingCall(m_options, u"unloadScript"_s, {m_scriptName}), QDBus::BlockWithGui, 500);
        return;
    }
    m_options.bus.asyncCall(scriptingCall(m_options, u"unloadScript"_s, {m_scriptName}), m_options.callTimeoutMs);
}

} // namespace murasaki
