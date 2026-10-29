#pragma once

#include "ptyprocess.h"
#include "steamcmdparser.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace murasaki {

inline constexpr int kMaxSteamCmdJobHistory = 20;

struct SteamCmdJob {
    enum State { Queued, Running, Done, Failed };
    QString appId;
    QString itemId;
    State state = Queued;
    double progress = 0;
    QString error;

    static QString stateName(State state);
    QJsonObject toJson() const;
};

// Optional Workshop downloads with the user's own Steam account through the official SteamCMD (see
// docs/design/specs/2026-10-17-murasaki-steamcmd-design.md). SteamCMD runs on a pseudo-terminal; the manager
// recognises its prompts (password, Steam Guard code, "confirm on your phone"), passes the answers it is given and
// does not store them. The password is never stored: SteamCMD keeps its own login token, and only the account name is kept
// (in the state file) to prefill the dialog.
//
// state():  "unavailable"  steamcmd is not installed
//           "logged-out"   no account is signed in
//           "logging-in"   a login is running; prompt() says what it waits for ("password", "guard", "confirm" or "")
//           "logged-in"    an account is signed in (SteamCMD holds its token; an expired one is found out by the
//                          first download, which then fails and returns the state to logged-out)
//
// Downloads queue and run one at a time into <installDir>/steamapps/workshop/content/<app>/<item>.
class SteamCmdManager : public QObject
{
    Q_OBJECT

public:
    // `installDir` is SteamCMD's force_install_dir, `statePath` the file that keeps the account name.
    SteamCmdManager(QString installDir, QString statePath, QObject *parent = nullptr);
    ~SteamCmdManager() override;

    // The program to run: $MURASAKI_STEAMCMD, else steamcmd on PATH, /usr/games/steamcmd or SteamCMD's own installer
    // dir. Empty when there is none.
    static QString findProgram();
    // <installDir>/steamapps/workshop/content/<appId> for each Workshop app Murasaki reads.
    static QString contentDir(const QString &installDir, const QString &appId);
    static bool isValidUser(const QString &user);
    static bool isValidItemId(const QString &itemId);
    static bool isValidAppId(const QString &appId);

    void setProgram(const QString &program); // tests use a fake
    QString program() const { return m_program; }
    QString installDir() const { return m_installDir; }
    void setLoginTimeoutMs(int ms) { m_loginTimeoutMs = ms; }
    // How long a running download may print nothing before it is stopped.
    void setStallTimeoutMs(int ms) { m_stallMs = ms; }

    bool available() const { return !m_program.isEmpty(); }
    QString state() const;
    QString user() const { return m_user; }
    QString prompt() const;
    QString message() const { return m_message; }
    QList<SteamCmdJob> jobs() const { return m_jobs; }
    QJsonObject toJson() const;

    // Starts a login (SteamCMD uses its cached token if it has one, else it prompts). False with *error when
    // steamcmd is missing, the name is invalid or something is already running.
    bool login(const QString &user, QString *error = nullptr);
    // Answers the prompt: kind is "password" or "guard". False when SteamCMD is not waiting for that.
    bool submit(const QString &kind, const QString &secret, QString *error = nullptr);
    void cancelLogin();
    // Signs out: SteamCMD's +logout, and the account name is forgotten.
    void logout();

    // Queues an item (appId 431960 or 3474900). False with *error for a bad id, when signed out or without steamcmd.
    // An item already queued or running is not queued twice.
    bool download(const QString &appId, const QString &itemId, QString *error = nullptr);
    // Cancels the running or queued download of the item; an empty id cancels them all.
    void cancel(const QString &itemId);

signals:
    void changed();

private:
    enum class Activity { Idle, LoggingIn, LoggingOut, Downloading };

    void setActivityIdle();
    // Hands the process (running or not) over to be stopped: its whole group gets SIGTERM, then SIGKILL, and nothing new
    // starts until every process of it is gone.
    void retire(PtyProcess *process);
    void reap();
    void startLogoutProcess(const QString &user);
    bool stopping() const { return !m_dying.isEmpty(); }
    void notify();
    void saveState() const;
    void loadState();
    void startLoginProcess();
    void onLoginOutput(const QByteArray &data);
    void onLoginEvent(const SteamCmdEvent &event);
    void onLoginFinished(int code, QProcess::ExitStatus status);
    void failLogin(const QString &reason);
    void runNextJob();
    void onJobOutput(const QByteArray &data);
    void onJobEvent(const SteamCmdEvent &event);
    void onJobFinished(int code, QProcess::ExitStatus status);
    void failCurrentJob(const QString &reason);
    void trimHistory();
    SteamCmdJob *runningJob();
    QProcessEnvironment environment() const;

    QString m_installDir;
    QString m_statePath;
    QString m_program;
    QString m_user;
    bool m_loggedIn = false;
    Activity m_activity = Activity::Idle;
    SteamCmdPrompt m_prompt = SteamCmdPrompt::None;
    QString m_message;
    QList<SteamCmdJob> m_jobs;
    std::unique_ptr<PtyProcess> m_process;
    LineSplitter m_splitter;
    QString m_lastLine;
    QList<PtyProcess *> m_dying;
    QString m_pendingLogoutUser;
    QTimer m_reapTimer;
    QTimer m_notifyTimer;
    QTimer m_watchdog; // login timeout and download stall
    int m_loginTimeoutMs = 180000;
    int m_stallMs = 120000;
    bool m_quitSent = false;
};

} // namespace murasaki
