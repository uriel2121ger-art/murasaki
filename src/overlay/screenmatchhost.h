#pragma once

#include "screenmatch/protocol.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

class QLocalSocket;

namespace murasaki {

class CrashLedger;

// Starts and watches murasaki-screenmatch: the helper behind `murasaki-sandbox --profile screenmatch`, with a socketpair
// for the line protocol of src/screenmatch/protocol.h and the PipeWire remote the daemon opened through the portal. Qt Core only. Like PuppetEngineHost it owns the crash policy: a crash is
// counted in the CrashLedger (keyed by the helper's binary, so a new binary starts a new count), and after CrashLedger::kMaxCrashes screen matching is blocked until the user turns it on again
// (resetCrashes()). Unlike the puppet engine it is NOT restarted by the host: the PipeWire remote it was given was used by the dead process and cannot serve another, so whoever owns the portal
// session opens a new remote and calls start() again when it chooses to (after `crashed`).
//
// What is a crash: a signal, or any exit status that is not one of the documented ones (exitcodes.h). What is not: a clean end, no PipeWire in the build, the desktop ending the share
// (StreamLost), and the sandbox helper's own 125 and 127.
//
// start() must be called from the thread the host lives in, which must be the main thread (the child asks for SIGKILL when the thread that forked it exits).
class ScreenMatchHost : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString helperPath;       // murasaki-screenmatch
        QString sandboxPath;      // murasaki-sandbox; empty: none
        bool requireSandbox = true;
        bool allowCapture = false;
        bool allowDmaBuf = false; // --buffers any (the SHM-only default is tried first)
        int longRunMs = 1800000;  // a helper that ran this long before it crashed starts the count again
        int stopGraceMs = 3000;
    };

    enum class State { Idle, Running, Blocked, Refused, NoPipeWire, StreamLost, Crashed };
    Q_ENUM(State)

    ScreenMatchHost(Config config, CrashLedger *ledger, QObject *parent = nullptr);
    ~ScreenMatchHost() override;

    // Starts the helper on `pipewireFd` (a duplicate is made; the caller keeps its own) for the monitor nodes of the cast. A blocked helper is not started.
    void start(int pipewireFd, const QList<unsigned> &nodes);
    // Asks the helper to quit (the `quit` command, then SIGTERM, then SIGKILL after Config::stopGraceMs). Returns at once.
    void stop();
    // One command to the helper (ignored when it is not running).
    void send(const screenmatch::Command &command);
    // The user turned screen matching on again: the crash count of the helper is forgotten and a Blocked host may start.
    void resetCrashes();

    State state() const { return m_state; }
    int crashes() const;
    int launches() const { return m_launches; }
    int badLines() const { return m_badLines; }
    // The command line of a start: program first. The descriptors are the numbers the child finds them under.
    static QStringList command(const Config &config, int channelFd, int pipewireFd, const QList<unsigned> &nodes);

signals:
    void reply(const murasaki::screenmatch::Reply &reply);
    void stateChanged(murasaki::ScreenMatchHost::State state);
    // A short reason: "screen: helper crashed", "screen: helper keeps crashing, turned off", "screen: stopped by the desktop", a refusal's reason.
    void degraded(const QString &why);

private:
    void onError(QProcess::ProcessError error);
    void onFinished(int code, QProcess::ExitStatus status);
    void onReadable();
    void setState(State state);

    Config m_config;
    CrashLedger *m_ledger;
    State m_state = State::Idle;
    // The two descriptors the child gets, ours until it has started (or failed to): closed once, by whoever gets there first (the started signal, a failed start, or stop()
    // before either came). Shared with the lambdas, which outlive neither the process nor this when they are connected to the process.
    struct ChildFds {
        int fd[2] = {-1, -1};
        void close();
    };
    std::shared_ptr<ChildFds> m_childFds;
    std::unique_ptr<QProcess> m_process;
    QPointer<QLocalSocket> m_channel;
    QList<QPointer<QProcess>> m_dying;
    screenmatch::LineBuffer m_lines;
    QElapsedTimer m_alive;
    int m_launches = 0;
    int m_badLines = 0;
    bool m_stopping = false;
};

} // namespace murasaki
