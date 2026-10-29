#pragma once

#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace murasaki {

// A child process on a pseudo-terminal, for programs that talk to a person (SteamCMD asks for passwords and codes on its
// terminal). The child gets a new session whose controlling terminal is the pty, with echo off, so what is written to it
// (a password) never comes back in the output. Output arrives in output(); write() feeds its input.
class PtyProcess : public QObject
{
    Q_OBJECT

public:
    explicit PtyProcess(QObject *parent = nullptr);
    ~PtyProcess() override;

    // False when the pty or the program cannot be started.
    bool start(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment);
    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    // Writes all of it, waiting (up to two seconds) for a full pty buffer to drain; false when it could not.
    bool write(const QByteArray &data);
    // SIGTERM to the child's whole process group (it is a session leader, so its helpers die with it), and SIGKILL to
    // the group if anything is still there after killDelayMs. Also cleans up a group that outlives its leader.
    void terminate(int killDelayMs = 2000);
    // No process of the child's group is left (true before start()).
    bool groupGone() const;

signals:
    void output(const QByteArray &data);
    // After the last of the output.
    void finished(int exitCode, QProcess::ExitStatus status);

private:
    void readAvailable();
    void closeMaster();

    QProcess m_process;
    std::unique_ptr<QSocketNotifier> m_notifier;
    QTimer m_killTimer;
    int m_master = -1;
    pid_t m_group = 0; // the child is a session leader: its process group id is its pid
};

} // namespace murasaki
