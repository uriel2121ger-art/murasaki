#include "screencastportal.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusReply>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDBusUnixFileDescriptor>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

const QString kService = u"org.freedesktop.portal.Desktop"_s;
const QString kPath = u"/org/freedesktop/portal/desktop"_s;
const QString kCastInterface = u"org.freedesktop.portal.ScreenCast"_s;
const QString kRequestInterface = u"org.freedesktop.portal.Request"_s;
const QString kSessionInterface = u"org.freedesktop.portal.Session"_s;

constexpr int kMaxCoordinate = 1000000;
constexpr int kMaxSide = 16384;
constexpr int kMaxText = 256;

bool plainText(const QString &s, int max = kMaxText)
{
    if (s.isEmpty() || s.size() > max)
        return false;
    for (const QChar c : s)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return false;
    return true;
}

QString malformed(const QString &detail)
{
    return QString(kScreenPortalMalformed) + u": "_s + detail;
}

// A session handle is the object path the portal makes for this client: .../session/<our unique name, sanitised>/<the token we asked for>. Anything else is not ours.
bool sessionPath(const QVariant &value, const QString &expected)
{
    QString path;
    if (value.metaType() == QMetaType::fromType<QString>())
        path = value.toString();
    else if (value.metaType() == QMetaType::fromType<QDBusObjectPath>())
        path = value.value<QDBusObjectPath>().path();
    else
        return false;
    return path == expected;
}

bool pair(const QVariant &value, int *a, int *b)
{
    if (!value.canConvert<QDBusArgument>())
        return false;
    const QDBusArgument arg = value.value<QDBusArgument>(); // (const: the read-side begin/end calls)
    if (arg.currentType() != QDBusArgument::StructureType || arg.currentSignature() != u"(ii)"_s)
        return false;
    arg.beginStructure();
    arg >> *a >> *b;
    arg.endStructure();
    return true;
}

// One source of Start's a(ua{sv}), as it is on the wire.
struct WireStream {
    quint32 node = 0;
    QVariantMap props;
};
const QDBusArgument &operator>>(const QDBusArgument &arg, WireStream &stream)
{
    arg.beginStructure();
    arg >> stream.node >> stream.props;
    arg.endStructure();
    return arg;
}

// The a(ua{sv}) of Start's results: node id and properties of each source. At most kMaxCastStreams are read; one more is a refusal, so a huge array costs nothing.
bool parseStreams(const QVariant &value, QList<ScreenCastPortal::Stream> *out, QString *why)
{
    if (!value.canConvert<QDBusArgument>()) {
        *why = u"streams is not an array"_s;
        return false;
    }
    const QDBusArgument arg = value.value<QDBusArgument>(); // (const: the read-side begin/end calls)
    if (arg.currentType() != QDBusArgument::ArrayType || arg.currentSignature() != u"a(ua{sv})"_s) {
        *why = u"streams has the wrong type"_s;
        return false;
    }
    arg.beginArray();
    while (!arg.atEnd()) {
        if (out->size() >= kMaxCastStreams) {
            *why = u"more than %1 streams"_s.arg(kMaxCastStreams);
            return false;
        }
        if (arg.currentSignature() != u"(ua{sv})"_s) {
            *why = u"a stream has the wrong type"_s;
            return false;
        }
        WireStream wire;
        arg >> wire;
        const quint32 node = wire.node;
        const QVariantMap &props = wire.props;
        if (node < 1 || node > 0x7ffffff0u) {
            *why = u"a stream has no usable node id"_s;
            return false;
        }
        ScreenCastPortal::Stream s;
        s.node = node;
        // Position and size are optional in the portal's documentation: both or neither. Without them a single stream is still the whole of its output; several need them to be told apart.
        const bool hasPosition = props.contains(u"position"_s), hasSize = props.contains(u"size"_s);
        if (hasPosition != hasSize) {
            *why = u"a stream has a position or a size but not both"_s;
            return false;
        }
        if (hasPosition) {
            int x = 0, y = 0, w = 0, h = 0;
            if (!pair(props.value(u"position"_s), &x, &y) || !pair(props.value(u"size"_s), &w, &h)) {
                *why = u"a stream's position and size are not (ii)"_s;
                return false;
            }
            // (explicit bounds: qAbs of the smallest int is not a number)
            if (w < 1 || h < 1 || w > kMaxSide || h > kMaxSide || x < -kMaxCoordinate || x > kMaxCoordinate || y < -kMaxCoordinate || y > kMaxCoordinate) {
                *why = u"a stream's geometry is out of range"_s;
                return false;
            }
            s.x = x;
            s.y = y;
            s.width = w;
            s.height = h;
            s.hasGeometry = true;
        }
        const QVariant sourceType = props.value(u"source_type"_s);
        if (sourceType.isValid() && (sourceType.metaType() != QMetaType::fromType<uint>() || sourceType.toUInt() != 1)) {
            *why = u"a stream is not a monitor"_s;
            return false;
        }
        const QVariant id = props.value(u"id"_s);
        if (id.isValid()) {
            if (id.metaType() != QMetaType::fromType<QString>() || !plainText(id.toString())) {
                *why = u"a stream's id is not text"_s;
                return false;
            }
            s.id = id.toString();
        }
        for (const ScreenCastPortal::Stream &other : std::as_const(*out))
            if (other.node == s.node) {
                *why = u"a node id twice"_s;
                return false;
            }
        out->append(s);
    }
    arg.endArray();
    if (out->size() > 1)
        for (const ScreenCastPortal::Stream &stream : std::as_const(*out))
            if (!stream.hasGeometry) {
                *why = u"several streams without position and size"_s;
                return false;
            }
    return true;
}

} // namespace

// Waits for the Response signal of one request, or its timeout. A Response counts only when it comes from the portal's pinned owner on a path this request listens to; one that
// beats the portal's first reply (when the owner is not known yet) is held until the owner is pinned.
class ScreenCastPortal::Waiter : public QObject
{
    Q_OBJECT

public:
    Waiter(QDBusConnection bus, std::function<QString()> owner, std::function<void(uint, const QVariantMap &, bool)> done, int timeoutMs, QObject *parent)
        : QObject(parent), m_bus(std::move(bus)), m_owner(std::move(owner)), m_done(std::move(done))
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(timeoutMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { finish(2, {}, true); });
        m_timer.start();
    }
    bool active() const { return bool(m_done); }
    void listen(const QString &path)
    {
        if (!m_done || m_paths.contains(path))
            return;
        m_paths.append(path);
        m_bus.connect(kService, path, kRequestInterface, u"Response"_s, this, SLOT(onResponse(uint, QVariantMap, QDBusMessage)));
    }
    void abandon()
    {
        m_done = nullptr;
        m_timer.stop();
        deleteLater();
    }
    // The owner has just been pinned: a Response held until now may be judged.
    void recheck()
    {
        if (!m_held)
            return;
        const Held held = *std::exchange(m_held, std::nullopt);
        judge(held.code, held.results, held.service, held.path);
    }

private slots:
    void onResponse(uint code, const QVariantMap &results, const QDBusMessage &message) { judge(code, results, message.service(), message.path()); }

private:
    struct Held {
        uint code;
        QVariantMap results;
        QString service, path;
    };
    void judge(uint code, const QVariantMap &results, const QString &service, const QString &path)
    {
        if (!m_done || !m_paths.contains(path))
            return;
        const QString owner = m_owner();
        if (owner.isEmpty()) {
            m_held = Held{code, results, service, path};
            return;
        }
        if (service != owner)
            return; // not the portal: ignored
        finish(code, results, false);
    }
    void finish(uint code, const QVariantMap &results, bool timedOut)
    {
        if (!m_done)
            return;
        auto done = std::exchange(m_done, nullptr);
        m_timer.stop();
        done(code, results, timedOut);
        deleteLater();
    }

    QDBusConnection m_bus;
    std::function<QString()> m_owner;
    std::function<void(uint, const QVariantMap &, bool)> m_done;
    QTimer m_timer;
    QStringList m_paths;
    std::optional<Held> m_held;
};

ScreenCastPortal::ScreenCastPortal(QDBusConnection bus, QObject *parent) : QObject(parent), m_bus(std::move(bus)) {}

ScreenCastPortal::~ScreenCastPortal()
{
    m_dying = true; // nobody is told about a state change while the object goes
    stop();
}

QString ScreenCastPortal::newToken(const char *what)
{
    return u"murasaki_%1_%2"_s.arg(QString::fromLatin1(what)).arg(QRandomGenerator::system()->generate64(), 0, 16);
}

void ScreenCastPortal::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    if (!m_dying)
        emit stateChanged(state);
}

void ScreenCastPortal::closeRemote()
{
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = -1;
}

int ScreenCastPortal::takeRemoteFd()
{
    return std::exchange(m_fd, -1);
}

void ScreenCastPortal::closeSession()
{
    // What is open on the portal's side is closed without waiting: the request (a dialog still on the screen goes away) and the session.
    if (!m_requestPath.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall(kService, m_requestPath, kRequestInterface, u"Close"_s);
        m_bus.asyncCall(close, 1000);
        m_requestPath.clear();
    }
    if (!m_session.isEmpty()) {
        m_bus.disconnect(kService, m_session, kSessionInterface, u"Closed"_s, this, SLOT(onSessionClosed(QVariantMap, QDBusMessage)));
        QDBusMessage close = QDBusMessage::createMethodCall(kService, m_session, kSessionInterface, u"Close"_s);
        m_bus.asyncCall(close, 1000);
        m_session.clear();
    }
}

void ScreenCastPortal::stop()
{
    ++m_generation; // every answer still on its way is for a run that is over
    delete m_openWatcher.data();
    for (const QPointer<Waiter> &w : std::as_const(m_waiters))
        if (w)
            w->abandon();
    m_waiters.clear();
    closeSession();
    closeRemote();
    m_streams.clear();
    m_reason.clear();
    setState(State::Idle);
}

void ScreenCastPortal::fail(quint64 generation, const QString &reason)
{
    if (generation != m_generation)
        return;
    ++m_generation;
    for (const QPointer<Waiter> &w : std::as_const(m_waiters))
        if (w)
            w->abandon();
    m_waiters.clear();
    closeSession();
    closeRemote();
    m_streams.clear();
    m_options.restoreToken.clear(); // spent or refused: not sent again by this object
    m_reason = reason;
    setState(State::Failed);
}

void ScreenCastPortal::start(const Options &options)
{
    stop();
    m_options = options;
    m_options.stepTimeoutMs = qBound(100, m_options.stepTimeoutMs, 60000);
    m_options.askTimeoutMs = qBound(100, m_options.askTimeoutMs, 600000);
    if (!plainText(m_options.restoreToken) || !m_options.remember)
        m_options.restoreToken.clear();
    setState(State::Asking);
    m_owner.clear();
    m_sessionToken.clear();
    m_cursorMode = -1;
    if (m_bus.isConnected() && m_bus.interface()) {
        // The portal's current owner of its name, pinned for this run. When nobody owns it yet the call below activates it, and the owner is pinned from its first reply.
        const QDBusReply<QString> owner = m_bus.interface()->serviceOwner(kService);
        if (owner.isValid())
            m_owner = owner.value();
    }
    if (!m_bus.isConnected()) {
        fail(m_generation, QString(kScreenPortalMissing));
        return;
    }
    createSession(m_generation);
}

void ScreenCastPortal::request(quint64 generation, QDBusMessage message, const QString &token, int timeoutMs, std::function<void(uint, const QVariantMap &)> next,
                               std::function<void(const QDBusError &)> onError, std::function<bool()> onDenied)
{
    // The Request object is named after our unique bus name and the handle token; listen before calling, because the answer can beat the method reply.
    QString sender = m_bus.baseService();
    sender.remove(u':').replace(u'.', u'_');
    const QString predicted = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, token);
    m_requestPath = predicted;
    auto *waiter = new Waiter(
        m_bus, [this] { return m_owner; },
        [this, generation, next = std::move(next), onDenied = std::move(onDenied)](uint code, const QVariantMap &results, bool timedOut) {
            if (generation != m_generation)
                return;
            if (!timedOut)
                m_requestPath.clear(); // answered: nothing to cancel. On a timeout the request is still open on the portal's side, and fail() closes it.
            if (timedOut) {
                fail(generation, QString(kScreenPortalSilent));
                return;
            }
            if (code != 0 && onDenied && onDenied())
                return;
            if (code == 1) {
                fail(generation, QString(kScreenPermissionRefused));
                return;
            }
            if (code != 0) {
                fail(generation, QString(kScreenPortalError));
                return;
            }
            next(code, results);
        },
        timeoutMs, this);
    m_waiters.removeAll(nullptr); // (the finished ones)
    m_waiters.append(waiter);
    waiter->listen(predicted);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, m_options.stepTimeoutMs), waiter);
    connect(watcher, &QDBusPendingCallWatcher::finished, waiter, [this, generation, waiter, watcher, predicted, onError = std::move(onError)] {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        // A reply that comes after its request is finished (the Response beat it) or after the run is over changes nothing.
        if (generation != m_generation || !waiter->active())
            return;
        if (reply.isError()) {
            const QDBusError error = reply.error();
            m_requestPath.clear(); // (no request was made)
            waiter->abandon();
            if (onError) {
                onError(error);
                return;
            }
            const bool missing = error.type() == QDBusError::ServiceUnknown || error.type() == QDBusError::NoServer || error.type() == QDBusError::UnknownObject
                || error.type() == QDBusError::UnknownInterface || error.type() == QDBusError::UnknownMethod;
            fail(generation, QString(missing ? kScreenPortalMissing : (error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout) ? kScreenPortalSilent : kScreenPortalError));
            return;
        }
        // A reply is the bus's own business (it only lets the destination of a call answer it); the signals are the ones that need a sender. If the portal was not running at start (D-Bus
        // activation) its owner is looked up now that it has answered, and a Response that arrived first is judged.
        if (m_owner.isEmpty() && m_bus.interface()) {
            const QDBusReply<QString> owner = m_bus.interface()->serviceOwner(kService);
            if (owner.isValid())
                m_owner = owner.value();
            if (m_owner.isEmpty()) {
                fail(generation, QString(kScreenPortalMissing));
                return;
            }
            waiter->recheck();
            if (!waiter->active())
                return;
        }
        // portals older than 0.9 do not honor the token: the path in the reply is listened to too, if it is a request path
        static const QRegularExpression requestPattern(u"\\A/org/freedesktop/portal/desktop/request/[A-Za-z0-9_]{1,100}/[A-Za-z0-9_]{1,100}\\z"_s);
        const QString returned = reply.value().path();
        if (requestPattern.match(returned).hasMatch()) {
            waiter->listen(returned);
            if (m_requestPath == predicted)
                m_requestPath = returned;
        }
    });
}

void ScreenCastPortal::createSession(quint64 generation)
{
    const QString token = newToken("create");
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kCastInterface, u"CreateSession"_s);
    m_sessionToken = newToken("session");
    message.setArguments({QVariantMap{{u"handle_token"_s, token}, {u"session_handle_token"_s, m_sessionToken}}});
    request(generation, message, token, m_options.stepTimeoutMs, [this, generation](uint, const QVariantMap &results) {
        QString sender = m_bus.baseService();
        sender.remove(u':').replace(u'.', u'_');
        const QString expected = u"/org/freedesktop/portal/desktop/session/%1/%2"_s.arg(sender, m_sessionToken);
        if (!sessionPath(results.value(u"session_handle"_s), expected)) {
            fail(generation, malformed(u"session_handle"_s));
            return;
        }
        m_session = expected;
        m_bus.connect(kService, m_session, kSessionInterface, u"Closed"_s, this, SLOT(onSessionClosed(QVariantMap, QDBusMessage)));
        selectSources(generation, !m_options.restoreToken.isEmpty());
    });
}

// The cursor is best left out of a reference: hidden (1), else delivered as metadata (4, not painted into the frame), else embedded (2). A portal that offers none of them gets no
// cursor_mode at all; one that does not say (an older portal, an error) is asked for hidden, as before. Looked up once per run.
void ScreenCastPortal::selectSources(quint64 generation, bool withToken)
{
    if (m_cursorMode >= 0) {
        sendSelect(generation, withToken);
        return;
    }
    QDBusMessage get = QDBusMessage::createMethodCall(kService, kPath, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    get.setArguments({kCastInterface, u"AvailableCursorModes"_s});
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(get, m_options.stepTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation, withToken, watcher] {
        watcher->deleteLater();
        if (generation != m_generation || m_state != State::Asking)
            return;
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        int mode = 1;
        if (reply.isValid()) {
            bool ok = false;
            const uint offered = reply.value().variant().toUInt(&ok);
            if (ok)
                mode = (offered & 1) ? 1 : (offered & 4) ? 4 : (offered & 2) ? 2 : 0;
        }
        m_cursorMode = mode;
        sendSelect(generation, withToken);
    });
}

void ScreenCastPortal::sendSelect(quint64 generation, bool withToken)
{
    const QString token = newToken("select");
    QVariantMap options{{u"handle_token"_s, token},
                        {u"types"_s, uint(1)},         // monitors
                        {u"multiple"_s, true}};        // every monitor the user ticks
    if (m_cursorMode > 0)
        options.insert(u"cursor_mode"_s, uint(m_cursorMode));
    if (m_options.remember) {
        options.insert(u"persist_mode"_s, uint(2)); // until revoked
        if (withToken)
            options.insert(u"restore_token"_s, m_options.restoreToken);
    }
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kCastInterface, u"SelectSources"_s);
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), options});
    // A stored token the portal does not take (an error, or a refused request) is dropped once and the dialog asks again.
    const auto retryWithoutToken = [this, generation, withToken] {
        if (!withToken)
            return false;
        m_options.restoreToken.clear();
        emit restoreTokenRefused();
        selectSources(generation, false);
        return true;
    };
    request(
        generation, message, token, m_options.stepTimeoutMs,
        [this, generation](uint, const QVariantMap &) { startCast(generation); },
        [this, generation, retryWithoutToken](const QDBusError &error) {
            if (retryWithoutToken())
                return;
            const bool silent = error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout;
            fail(generation, QString(silent ? kScreenPortalSilent : kScreenPortalError));
        },
        [retryWithoutToken] { return retryWithoutToken(); });
}

void ScreenCastPortal::startCast(quint64 generation)
{
    const QString token = newToken("start");
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kCastInterface, u"Start"_s);
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), QString(), QVariantMap{{u"handle_token"_s, token}}});
    // The dialog is open: this is where a person is waited for.
    request(generation, message, token, m_options.askTimeoutMs, [this, generation](uint, const QVariantMap &results) {
        QList<Stream> streams;
        QString why;
        if (!parseStreams(results.value(u"streams"_s), &streams, &why)) {
            fail(generation, malformed(why));
            return;
        }
        if (streams.isEmpty()) {
            fail(generation, QString(kScreenNoMonitors));
            return;
        }
        m_streams = streams;
        if (m_options.remember) {
            const QVariant token = results.value(u"restore_token"_s);
            if (token.metaType() == QMetaType::fromType<QString>() && plainText(token.toString())) {
                m_options.restoreToken = token.toString();
                emit restoreToken(token.toString());
            }
        }
        openRemote(generation);
    });
}

void ScreenCastPortal::openRemote(quint64 generation)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kCastInterface, u"OpenPipeWireRemote"_s);
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), QVariantMap{}});
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, m_options.stepTimeoutMs), this);
    m_openWatcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation, watcher] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            const QDBusError error(reply);
            fail(generation, QString(error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout ? kScreenPortalSilent : kScreenPortalError));
            return;
        }
        if (reply.arguments().size() != 1 || !reply.arguments().constFirst().canConvert<QDBusUnixFileDescriptor>()) {
            fail(generation, malformed(u"OpenPipeWireRemote did not return a descriptor"_s));
            return;
        }
        const QDBusUnixFileDescriptor given = reply.arguments().constFirst().value<QDBusUnixFileDescriptor>();
        if (!given.isValid()) {
            fail(generation, malformed(u"OpenPipeWireRemote returned an invalid descriptor"_s));
            return;
        }
        // Our own duplicate, close-on-exec, numbered above the standard three: the one QDBusUnixFileDescriptor holds goes with the message.
        const int own = ::fcntl(given.fileDescriptor(), F_DUPFD_CLOEXEC, 3);
        if (own < 0) {
            fail(generation, QString(kScreenPortalError));
            return;
        }
        m_fd = own;
        m_requestPath.clear();
        setState(State::Watching);
    });
}

void ScreenCastPortal::onSessionClosed(const QVariantMap &, const QDBusMessage &message)
{
    if (m_state != State::Asking && m_state != State::Watching)
        return;
    if (message.service() != m_owner || message.path() != m_session)
        return; // not the portal, or not our session
    // The desktop (or the user from the panel's indicator) ended the share. A close before the answer to Start is also the end of that attempt. The session is gone: nothing is asked of it.
    m_bus.disconnect(kService, m_session, kSessionInterface, u"Closed"_s, this, SLOT(onSessionClosed(QVariantMap, QDBusMessage)));
    m_session.clear();
    m_requestPath.clear();
    fail(m_generation, QString(kScreenStoppedByDesktop));
}

} // namespace murasaki

#include "screencastportal.moc"
