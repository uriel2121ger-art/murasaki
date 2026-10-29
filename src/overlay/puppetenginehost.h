#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace murasaki {

class CrashLedger;

// Starts and watches the puppet engine of one pet (docs/pets/puppets.md, section 19): `murasaki-puppet --engine` behind `murasaki-sandbox --profile puppet`, with a bridge socketpair whose consumer end
// goes to the caller. It owns the restart policy: a crash restarts the engine; the third crash of the same model blocks it, across restarts of the overlay (the CrashLedger). A refusal of the model, no
// GPU, or no sandbox is not a crash: it is reported and not retried. Qt Core only; the pet window (task 8) connects the consumer end to a BridgeItem or its own client.
//
// Thread: start() must be called from the main thread (the one the host lives in). The child asks for SIGKILL when its parent dies with PR_SET_PDEATHSIG, and Linux sends that when the *thread* that forked
// the child exits, not the process: an engine started from a worker thread would be killed when that worker ended. start() refuses from any other thread.
//
// Nothing here blocks: starting and stopping are asynchronous (a start that fails is reported by a signal, a stop sends SIGTERM and kills the engine if it is still there after a grace period).
//
// What counts as a crash: the process crashed (a signal), or exited with kExitFailure, kExitUsage or kExitDeviceLost, or with a status that is none of the documented ones. What does not:
// kExitOk, kExitModelRefused, kExitNoGpu, and the sandbox helper's own 125 and 127.
class PuppetEngineHost : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString enginePath;       // murasaki-puppet
        QString sandboxPath;      // murasaki-sandbox; empty: none
        bool requireSandbox = true; // refuse to run unconfined (the setting requireSandbox); false runs the engine directly when there is no helper
        QString corePath;         // the optional Cubism runtime the profile may read (nothing loads it yet)
        int fps = 60;
        quint64 seed = 1;
        int restartDelayMs = 250; // after a crash, before the engine is started again
        int longRunMs = 1800000;  // an engine that ran this long before it crashed does not count toward blocking its model (kEngineLongRunSeconds)
        int stopGraceMs = 3000;   // stop(): after SIGTERM, how long before SIGKILL
    };

    enum class State { Idle, Running, Restarting, Blocked, Refused, NoGpu };
    Q_ENUM(State)

    // `ledger` outlives the host. Its key for a model is the path of the model file.
    PuppetEngineHost(Config config, CrashLedger *ledger, QObject *parent = nullptr);
    ~PuppetEngineHost() override;

    // Starts the engine for the model `itemDir/file` of the pet `petId`, picture `size` in pixels. A blocked model is not started.
    void start(const QString &petId, const QString &itemDir, const QString &file, QSize size);
    // The engine is asked to stop (SIGTERM, then SIGKILL after Config::stopGraceMs; its socket closes) and nothing restarts. Returns at once. A start() that follows does not leave two engines alive: it kills
    // (SIGKILL) any that is still going and waits for it, a few milliseconds, before it launches the next.
    void stop();
    // The overlay has the first picture of this run: the opening marker of the pet is cleared, and the clock of "it ran a long time" (Config::longRunMs) starts.
    void firstFrameShown();

    State state() const { return m_state; }
    int launches() const { return m_launches; }
    int crashes() const;
    // The command line of a start: the program, then its arguments. `bridgeFd` is the descriptor number the child finds the bridge on.
    static QStringList command(const Config &config, const QString &itemDir, const QString &file, QSize size, int bridgeFd);

signals:
    // The consumer end of a new bridge connection; the receiver owns the descriptor. Emitted at every (re)start, before the engine runs. With nothing connected to it the descriptor is closed instead (and
    // the engine, seeing its socket closed, ends): no one is there to take the pictures.
    void consumerFd(int fd);
    // A name of src/core/puppetnames.h ("puppet: engine restarted", "puppet: blocked after crashes", "puppet: no GPU") or a short reason for a refusal.
    void degraded(const QString &name);
    void stateChanged(murasaki::PuppetEngineHost::State state);

private:
    void launch();
    void onError(QProcess::ProcessError error);
    void onFinished(int code, QProcess::ExitStatus status);
    void setState(State state);
    QString modelPath() const;

    Config m_config;
    CrashLedger *m_ledger;
    QString m_petId, m_itemDir, m_file;
    QSize m_size;
    State m_state = State::Idle;
    std::unique_ptr<QProcess> m_process;
    QTimer m_restart;
    QList<QPointer<QProcess>> m_dying; // engines asked to stop that have not gone yet
    QElapsedTimer m_alive; // since the first picture of this run (firstFrameShown)
    int m_launches = 0;
    bool m_stopping = false;
    bool m_marked = false;
};

} // namespace murasaki
