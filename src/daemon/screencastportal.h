#pragma once

// The client of the desktop's screen cast portal: org.freedesktop.portal.ScreenCast, driven as the portal
// documents it: CreateSession, SelectSources (monitors, several, cursor hidden), Start (the desktop's own dialog asks the user), then OpenPipeWireRemote, whose descriptor goes to the matching
// helper. Nothing here reads a pixel, and nothing here is wired into the trigger engine yet (task 5).
//
// A small state machine that task 5 shows to the user:
//   Idle      nothing asked, or stopped by the caller
//   Asking    the portal is being talked to; the user's dialog is open or about to be
//   Watching  the desktop said yes: streams() is known and takeRemoteFd() has the PipeWire remote
//   Failed    reason() names why; the only way out is start() again (there is no automatic restart)
// Every failure has a name (below), so the GUI and `murasakictl` can say it and the tests can pin it.
//
// Who is the portal: the unique owner of org.freedesktop.portal.Desktop when start() is called (when nobody owns the name yet the first call activates it and the owner is looked up once it has
// answered). Responses and Session.Closed from any other sender are ignored; method replies are the bus's own business (only the destination of a call can answer it). A process of the same user that already owns the name when we start is, as always on a session bus, the portal.
//
// The descriptor of the PipeWire remote is a duplicate that is ours alone (close-on-exec), handed over by takeRemoteFd() exactly once; whatever is not taken is closed by stop(), by a failure and
// by the destructor. The portal's replies are untrusted input: every type, count and range is checked (at most 8 streams, geometry in range, names and tokens bounded) and a reply that is not
// as documented fails the whole request with `screen: portal reply malformed` and the detail.

#include <QDBusConnection>
#include <QDBusMessage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QDBusError;

namespace murasaki {

inline constexpr QLatin1String kScreenPermissionRefused("screen: permission refused");    // the user said no (or closed the dialog)
inline constexpr QLatin1String kScreenStoppedByDesktop("screen: stopped by the desktop"); // the session was closed under us
inline constexpr QLatin1String kScreenPortalMissing("screen: portal not available");      // no ScreenCast portal on this session
inline constexpr QLatin1String kScreenPortalError("screen: portal error");                // the portal answered with an error
inline constexpr QLatin1String kScreenPortalSilent("screen: portal did not answer");      // a step, or the dialog, took too long
inline constexpr QLatin1String kScreenPortalMalformed("screen: portal reply malformed");  // followed by ": detail"
inline constexpr QLatin1String kScreenNoMonitors("screen: no monitor shared");            // the user shared nothing we can watch

inline constexpr int kMaxCastStreams = 8;

class ScreenCastPortal : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Asking, Watching, Failed };
    Q_ENUM(State)

    struct Options {
        bool remember = false;      // the setting "remember my choice": persist_mode 2 and the restore token. Off: neither is ever sent, and a token in a reply is ignored.
        QString restoreToken;       // the token kept from an earlier run (only used with `remember`). A token is single use: after a run that sent it the caller keeps only what restoreToken() announces.
        int stepTimeoutMs = 10000;  // CreateSession, SelectSources and OpenPipeWireRemote
        int askTimeoutMs = 120000;  // Start: the dialog waits for a person
    };

    // One monitor of the cast, in the desktop's logical coordinates (the portal's `position` and `size`).
    struct Stream {
        quint32 node = 0; // the PipeWire node id
        int x = 0, y = 0, width = 0, height = 0;
        QString id;       // the portal's own id for the source, when it gave one
        bool hasGeometry = false; // position and size came from the portal (several streams always have them; a single stream may not: then it is the whole of its output)
        bool operator==(const Stream &other) const = default;
    };

    explicit ScreenCastPortal(QDBusConnection bus, QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    // Opens the portal dialog. Any earlier run is stopped first. The result is stateChanged(): Watching or Failed.
    void start(const Options &options);
    // Cancels whatever is open (the dialog, the session), closes an untaken descriptor and goes to Idle. No reason.
    void stop();

    State state() const { return m_state; }
    QString reason() const { return m_reason; } // set in Failed
    QList<Stream> streams() const { return m_streams; }
    // The PipeWire remote, once, or -1. Close-on-exec. The caller owns it.
    int takeRemoteFd();
    // Whether a descriptor is waiting to be taken (its number, or -1), for diagnostics and tests.
    int pendingRemoteFd() const { return m_fd; }

signals:
    void stateChanged(murasaki::ScreenCastPortal::State state);
    // The portal gave a restore token and `remember` is on: the caller stores it. Never emitted with `remember` off.
    void restoreToken(const QString &token);
    // The portal did not accept the stored token; the request went on without it (the dialog asks again).
    void restoreTokenRefused();

private slots:
    void onSessionClosed(const QVariantMap &details, const QDBusMessage &message);

private:
    class Waiter;
    void createSession(quint64 generation);
    void selectSources(quint64 generation, bool withToken);
    void sendSelect(quint64 generation, bool withToken);
    void startCast(quint64 generation);
    void openRemote(quint64 generation);
    void request(quint64 generation, QDBusMessage message, const QString &token, int timeoutMs, std::function<void(uint, const QVariantMap &)> next, std::function<void(const QDBusError &)> onError = {}, std::function<bool()> onDenied = {});
    void fail(quint64 generation, const QString &reason);
    void setState(State state);
    void closeRemote();
    void closeSession();
    QString newToken(const char *what);

    QDBusConnection m_bus;
    State m_state = State::Idle;
    QString m_reason;
    Options m_options;
    QString m_session;
    QString m_requestPath;
    QList<Stream> m_streams;
    QList<QPointer<Waiter>> m_waiters;
    QPointer<QObject> m_openWatcher;
    QString m_owner;        // the portal's unique name for this run; Responses, Closed and replies from anyone else are ignored
    QString m_sessionToken;
    int m_cursorMode = -1;  // the cursor mode asked for: -1 not looked up yet, 0 none offered (the option is left out)
    bool m_dying = false;
    int m_fd = -1;
    quint64 m_generation = 0;
};

} // namespace murasaki
