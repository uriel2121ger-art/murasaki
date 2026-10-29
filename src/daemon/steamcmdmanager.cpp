#include "steamcmdmanager.h"

#include "jsonfile.h"
#include "steamlocator.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {
const QString kWallpaperEngineApp = u"431960"_s;
const QString kAnimaEngineApp = u"3474900"_s;
} // namespace

QString SteamCmdJob::stateName(State state)
{
    switch (state) {
    case Queued:
        return u"queued"_s;
    case Running:
        return u"running"_s;
    case Done:
        return u"done"_s;
    case Failed:
        break;
    }
    return u"failed"_s;
}

QJsonObject SteamCmdJob::toJson() const
{
    return QJsonObject{{u"appId"_s, appId},
                       {u"itemId"_s, itemId},
                       {u"state"_s, stateName(state)},
                       {u"progress"_s, progress},
                       {u"error"_s, error}};
}

SteamCmdManager::SteamCmdManager(QString installDir, QString statePath, QObject *parent)
    : QObject(parent)
    , m_installDir(std::move(installDir))
    , m_statePath(std::move(statePath))
    , m_program(findProgram())
{
    m_notifyTimer.setSingleShot(true);
    m_notifyTimer.setInterval(100); // progress arrives much faster than a window needs it
    connect(&m_notifyTimer, &QTimer::timeout, this, &SteamCmdManager::changed);
    m_reapTimer.setInterval(100);
    connect(&m_reapTimer, &QTimer::timeout, this, &SteamCmdManager::reap);
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_activity == Activity::LoggingIn)
            failLogin(tr("The login took too long and was stopped."));
        else if (m_activity == Activity::Downloading)
            failCurrentJob(tr("SteamCMD stopped answering."));
        else if (m_process)
            m_process->terminate();
    });
    loadState();
}

SteamCmdManager::~SteamCmdManager()
{
    qDeleteAll(m_dying); // the destructor of each kills what is left of its group
}

QString SteamCmdManager::findProgram()
{
    const auto usable = [](const QString &path) {
        const QFileInfo info(path);
        return info.isFile() && info.isExecutable();
    };
    const QString fromEnv = qEnvironmentVariable("MURASAKI_STEAMCMD");
    if (!fromEnv.isEmpty())
        return usable(fromEnv) ? fromEnv : QString();
    if (const QString onPath = QStandardPaths::findExecutable(u"steamcmd"_s); !onPath.isEmpty())
        return onPath;
    for (const QString &candidate :
         {u"/usr/games/steamcmd"_s, QDir::homePath() + u"/.steam/steamcmd/steamcmd.sh"_s,
          QDir::homePath() + u"/steamcmd/steamcmd.sh"_s}) {
        if (usable(candidate))
            return candidate;
    }
    return {};
}

QString SteamCmdManager::contentDir(const QString &installDir, const QString &appId)
{
    return SteamLocator::steamCmdContentDir(installDir, appId);
}

bool SteamCmdManager::isValidUser(const QString &user)
{
    // Starts with a letter or digit: never something that reads as an option or a command.
    static const QRegularExpression pattern(u"^[A-Za-z0-9][A-Za-z0-9_.@-]{0,63}$"_s);
    return pattern.match(user).hasMatch();
}

bool SteamCmdManager::isValidItemId(const QString &itemId)
{
    static const QRegularExpression pattern(u"^[0-9]{1,20}$"_s);
    return pattern.match(itemId).hasMatch();
}

bool SteamCmdManager::isValidAppId(const QString &appId)
{
    return appId == kWallpaperEngineApp || appId == kAnimaEngineApp;
}

void SteamCmdManager::setProgram(const QString &program)
{
    m_program = program;
    notify();
}

QString SteamCmdManager::state() const
{
    if (!available())
        return u"unavailable"_s;
    if (m_activity == Activity::LoggingIn)
        return u"logging-in"_s;
    return m_loggedIn ? u"logged-in"_s : u"logged-out"_s;
}

QString SteamCmdManager::prompt() const
{
    switch (m_prompt) {
    case SteamCmdPrompt::Password:
        return u"password"_s;
    case SteamCmdPrompt::Guard:
        return u"guard"_s;
    case SteamCmdPrompt::Confirm:
        return u"confirm"_s;
    case SteamCmdPrompt::None:
        break;
    }
    return {};
}

QJsonObject SteamCmdManager::toJson() const
{
    QJsonArray jobs;
    for (const SteamCmdJob &job : m_jobs)
        jobs.append(job.toJson());
    return QJsonObject{{u"available"_s, available()},
                       {u"program"_s, m_program},
                       {u"state"_s, state()},
                       {u"user"_s, m_user},
                       {u"prompt"_s, prompt()},
                       {u"message"_s, m_message},
                       {u"jobs"_s, jobs}};
}

void SteamCmdManager::notify()
{
    trimHistory();
    if (!m_notifyTimer.isActive())
        m_notifyTimer.start();
}

void SteamCmdManager::loadState()
{
    const JsonFileRead read = readJsonObjectFile(m_statePath);
    if (!read.exists || !read.problem.isEmpty())
        return; // nothing worth quarantining: it only holds a name
    const QString user = read.root.value(u"user"_s).toString();
    if (isValidUser(user)) {
        m_user = user;
        m_loggedIn = read.root.value(u"loggedIn"_s).toBool();
    }
}

void SteamCmdManager::saveState() const
{
    if (m_user.isEmpty() && !QFileInfo::exists(m_statePath))
        return;
    writeJsonObjectFile(m_statePath, QJsonObject{{u"version"_s, 1}, {u"user"_s, m_user}, {u"loggedIn"_s, m_loggedIn}});
}

QProcessEnvironment SteamCmdManager::environment() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(u"TERM"_s, u"dumb"_s); // no colours or cursor movement in what is parsed
    return env;
}

void SteamCmdManager::setActivityIdle()
{
    m_activity = Activity::Idle;
    m_prompt = SteamCmdPrompt::None;
    m_quitSent = false;
    m_watchdog.stop();
    if (m_process) // often called from the process's own finished(): it is retired once that signal has returned
        retire(m_process.release());
    m_splitter = LineSplitter();
}

void SteamCmdManager::retire(PtyProcess *process)
{
    process->disconnect(this);
    process->terminate();
    if (!process->isRunning() && process->groupGone()) { // already over: nothing to wait for
        process->deleteLater();
        return;
    }
    m_dying.append(process);
    if (!m_reapTimer.isActive())
        m_reapTimer.start();
}

void SteamCmdManager::reap()
{
    for (qsizetype i = m_dying.size() - 1; i >= 0; --i) {
        PtyProcess *process = m_dying.at(i);
        if (!process->isRunning() && process->groupGone()) {
            m_dying.removeAt(i);
            process->deleteLater();
        }
    }
    if (!m_dying.isEmpty())
        return;
    m_reapTimer.stop();
    notify();
    if (!m_pendingLogoutUser.isEmpty()) { // a sign-out that waited for the groups it stopped
        const QString user = std::exchange(m_pendingLogoutUser, QString());
        startLogoutProcess(user);
        return;
    }
    runNextJob(); // what waited for the last group to go
}

// ---- login ---------------------------------------------------------------------------------------------

bool SteamCmdManager::login(const QString &user, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!available())
        return fail(tr("SteamCMD is not installed."));
    if (!isValidUser(user))
        return fail(tr("The account name is not valid."));
    if (m_activity != Activity::Idle || stopping())
        return fail(tr("SteamCMD is busy."));
    m_user = user;
    m_loggedIn = false;
    m_message.clear();
    startLoginProcess();
    return true;
}

void SteamCmdManager::startLoginProcess()
{
    QDir().mkpath(m_installDir);
    m_activity = Activity::LoggingIn;
    m_prompt = SteamCmdPrompt::None;
    m_quitSent = false;
    m_splitter = LineSplitter();
    m_process = std::make_unique<PtyProcess>();
    connect(m_process.get(), &PtyProcess::output, this, &SteamCmdManager::onLoginOutput);
    connect(m_process.get(), &PtyProcess::finished, this, &SteamCmdManager::onLoginFinished);
    // The password is never an argument: SteamCMD asks for it on its terminal when it has no token.
    if (!m_process->start(m_program, {u"+force_install_dir"_s, m_installDir, u"+login"_s, m_user}, environment())) {
        setActivityIdle();
        m_message = tr("SteamCMD could not be started.");
        notify();
        return;
    }
    m_watchdog.start(m_loginTimeoutMs);
    notify();
}

void SteamCmdManager::onLoginOutput(const QByteArray &data)
{
    const auto [lines, pending] = m_splitter.feed(data);
    for (const QString &line : lines) {
        onLoginEvent(parseSteamCmdLine(line));
        if (m_activity != Activity::LoggingIn)
            return; // the login ended in this chunk: what follows belongs to a finished attempt
    }
    const SteamCmdPrompt prompt = detectPrompt(pending);
    if (prompt != SteamCmdPrompt::None)
        onLoginEvent(SteamCmdEvent{SteamCmdEvent::Prompt, prompt, {}, {}, 0});
}

void SteamCmdManager::onLoginEvent(const SteamCmdEvent &event)
{
    if (m_activity != Activity::LoggingIn)
        return;
    switch (event.kind) {
    case SteamCmdEvent::Prompt:
        if (m_prompt != event.prompt) {
            m_prompt = event.prompt;
            // A prompt after something was sent (a second password prompt) means the first answer was refused.
            notify();
        }
        break;
    case SteamCmdEvent::LoggedIn:
        m_loggedIn = true;
        m_prompt = SteamCmdPrompt::None;
        m_message.clear();
        saveState();
        notify();
        if (m_process && !m_quitSent) {
            m_quitSent = true;
            m_process->write("quit\n"); // the login is done; SteamCMD keeps the token
        }
        break;
    case SteamCmdEvent::LoginFailed:
        failLogin(event.text);
        break;
    default:
        break;
    }
}

void SteamCmdManager::failLogin(const QString &reason)
{
    m_message = reason;
    m_loggedIn = false;
    m_prompt = SteamCmdPrompt::None;
    saveState();
    if (m_process) {
        retire(m_process.release());
    }
    setActivityIdle();
    notify();
    runNextJob();
}

void SteamCmdManager::onLoginFinished(int code, QProcess::ExitStatus)
{
    const bool wasLoggedIn = m_loggedIn && m_activity == Activity::LoggingIn;
    if (m_activity == Activity::LoggingIn && !wasLoggedIn) {
        m_message = tr("SteamCMD ended before the login finished (code %1).").arg(code);
        m_loggedIn = false;
        saveState();
    }
    setActivityIdle();
    notify();
    runNextJob();
}

bool SteamCmdManager::submit(const QString &kind, const QString &secret, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const SteamCmdPrompt wanted = kind == u"password" ? SteamCmdPrompt::Password
        : kind == u"guard"                              ? SteamCmdPrompt::Guard
                                                        : SteamCmdPrompt::None;
    if (wanted == SteamCmdPrompt::None)
        return fail(tr("The kind must be password or guard."));
    if (m_activity != Activity::LoggingIn || m_prompt != wanted || !m_process)
        return fail(tr("SteamCMD is not asking for that."));
    if (secret.isEmpty() || secret.contains(u'\n') || secret.contains(u'\r') || secret.size() > 256)
        return fail(tr("That cannot be entered."));
    QByteArray bytes = secret.toUtf8();
    bytes.append('\n');
    const bool written = m_process->write(bytes);
    // The daemon's own copy of the secret does not outlive the write (the caller's QString is not ours to wipe).
    bytes.fill('\0');
    bytes.clear();
    if (!written)
        return fail(tr("The answer could not be sent to SteamCMD."));
    m_prompt = SteamCmdPrompt::None; // answered; what comes next is SteamCMD's reply
    m_splitter = LineSplitter();     // the prompt's text is spent: it must not read as a new prompt
    notify();
    return true;
}

void SteamCmdManager::cancelLogin()
{
    if (m_activity != Activity::LoggingIn)
        return;
    m_loggedIn = false;
    m_message.clear();
    saveState();
    if (m_process) {
        retire(m_process.release());
    }
    setActivityIdle();
    notify();
    runNextJob();
}

void SteamCmdManager::logout()
{
    // Whatever runs stops: a download needs the account that is going away.
    cancel({});
    cancelLogin();
    const QString user = m_user;
    m_user.clear();
    m_loggedIn = false;
    m_message.clear();
    if (QFileInfo::exists(m_statePath))
        QFile::remove(m_statePath);
    notify();
    if (!available() || user.isEmpty() || m_activity != Activity::Idle)
        return;
    if (stopping()) { // the groups stopped above are still going: SteamCMD must not run twice at once
        m_pendingLogoutUser = user;
        return;
    }
    startLogoutProcess(user);
}

void SteamCmdManager::startLogoutProcess(const QString &user)
{
    // SteamCMD's own +logout, so that it holds no session for the account. It must not stop to ask for a password.
    m_activity = Activity::LoggingOut;
    m_splitter = LineSplitter();
    m_process = std::make_unique<PtyProcess>();
    connect(m_process.get(), &PtyProcess::output, this, [this](const QByteArray &data) {
        const auto [lines, pending] = m_splitter.feed(data);
        if (detectPrompt(pending) != SteamCmdPrompt::None && m_process)
            m_process->terminate(); // no cached session to end
    });
    connect(m_process.get(), &PtyProcess::finished, this, [this] {
        setActivityIdle();
        notify();
        runNextJob();
    });
    if (!m_process->start(m_program, {u"+login"_s, user, u"+logout"_s, u"+quit"_s}, environment())) {
        setActivityIdle();
        return;
    }
    m_watchdog.start(60000);
}

// ---- downloads -----------------------------------------------------------------------------------------

bool SteamCmdManager::download(const QString &appId, const QString &itemId, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!available())
        return fail(tr("SteamCMD is not installed."));
    if (!isValidAppId(appId))
        return fail(tr("Only Wallpaper Engine (431960) and Anima Engine (3474900) items can be downloaded."));
    if (!isValidItemId(itemId))
        return fail(tr("The item id is not valid."));
    if (!m_loggedIn)
        return fail(tr("Sign in with your Steam account first."));
    for (const SteamCmdJob &job : std::as_const(m_jobs)) {
        if (job.itemId == itemId && (job.state == SteamCmdJob::Queued || job.state == SteamCmdJob::Running))
            return true; // already on its way
    }
    // A finished entry of the same item makes way for the new one.
    m_jobs.removeIf([&](const SteamCmdJob &job) { return job.itemId == itemId; });
    SteamCmdJob job;
    job.appId = appId;
    job.itemId = itemId;
    m_jobs.append(job);
    trimHistory();
    notify();
    runNextJob();
    return true;
}

SteamCmdJob *SteamCmdManager::runningJob()
{
    for (SteamCmdJob &job : m_jobs) {
        if (job.state == SteamCmdJob::Running)
            return &job;
    }
    return nullptr;
}

void SteamCmdManager::trimHistory()
{
    int finished = 0;
    for (qsizetype i = m_jobs.size() - 1; i >= 0; --i) {
        const auto state = m_jobs.at(i).state;
        if ((state == SteamCmdJob::Done || state == SteamCmdJob::Failed) && ++finished > kMaxSteamCmdJobHistory)
            m_jobs.removeAt(i);
    }
}

void SteamCmdManager::runNextJob()
{
    if (m_activity != Activity::Idle || !available() || !m_loggedIn || runningJob() || stopping()
        || !m_pendingLogoutUser.isEmpty())
        return;
    for (SteamCmdJob &job : m_jobs) {
        if (job.state != SteamCmdJob::Queued)
            continue;
        QDir().mkpath(m_installDir);
        job.state = SteamCmdJob::Running;
        m_activity = Activity::Downloading;
        m_splitter = LineSplitter();
        m_lastLine.clear();
        m_process = std::make_unique<PtyProcess>();
        connect(m_process.get(), &PtyProcess::output, this, &SteamCmdManager::onJobOutput);
        connect(m_process.get(), &PtyProcess::finished, this, &SteamCmdManager::onJobFinished);
        const QStringList arguments{u"+force_install_dir"_s, m_installDir, u"+login"_s,  m_user,
                                    u"+workshop_download_item"_s, job.appId, job.itemId, u"+quit"_s};
        if (!m_process->start(m_program, arguments, environment())) {
            failCurrentJob(tr("SteamCMD could not be started."));
            return;
        }
        m_watchdog.start(m_stallMs);
        notify();
        return;
    }
}

void SteamCmdManager::onJobOutput(const QByteArray &data)
{
    m_watchdog.start(m_stallMs); // output is life
    const auto [lines, pending] = m_splitter.feed(data);
    for (const QString &line : lines) {
        const QString clean = stripTerminalCodes(line).trimmed();
        if (!clean.isEmpty())
            m_lastLine = clean;
        onJobEvent(parseSteamCmdLine(line));
        if (m_activity != Activity::Downloading)
            return; // the job ended in this chunk (a failure kills the process): drop the rest
    }
    // A prompt during a download: the cached login is no good, and nobody is here to answer.
    if (detectPrompt(pending) != SteamCmdPrompt::None)
        onJobEvent(SteamCmdEvent{SteamCmdEvent::LoginRequired, SteamCmdPrompt::None, {}, {}, 0});
}

void SteamCmdManager::onJobEvent(const SteamCmdEvent &event)
{
    SteamCmdJob *job = runningJob();
    if (!job || m_activity != Activity::Downloading)
        return;
    switch (event.kind) {
    case SteamCmdEvent::Progress:
        if (event.progress != job->progress) {
            job->progress = event.progress;
            notify();
        }
        break;
    case SteamCmdEvent::Downloaded:
        if (event.itemId == job->itemId) {
            job->state = SteamCmdJob::Done;
            job->progress = 1;
            notify();
        }
        break;
    case SteamCmdEvent::DownloadFailed:
        if (event.itemId == job->itemId)
            failCurrentJob(event.text);
        break;
    case SteamCmdEvent::LoginRequired:
    case SteamCmdEvent::LoginFailed:
        m_loggedIn = false;
        m_message = tr("Your Steam session has expired. Sign in again.");
        saveState();
        failCurrentJob(m_message);
        break;
    default:
        break;
    }
}

void SteamCmdManager::failCurrentJob(const QString &reason)
{
    if (SteamCmdJob *job = runningJob()) {
        job->state = SteamCmdJob::Failed;
        job->error = reason;
    }
    if (m_process) {
        retire(m_process.release());
    }
    setActivityIdle();
    notify();
    if (m_loggedIn) {
        runNextJob();
    } else { // the rest cannot run without an account
        for (SteamCmdJob &job : m_jobs) {
            if (job.state == SteamCmdJob::Queued) {
                job.state = SteamCmdJob::Failed;
                job.error = m_message;
            }
        }
    }
}

void SteamCmdManager::onJobFinished(int code, QProcess::ExitStatus)
{
    if (SteamCmdJob *job = runningJob()) {
        job->state = SteamCmdJob::Failed;
        job->error = m_lastLine.isEmpty() ? tr("SteamCMD ended without downloading it (code %1).").arg(code)
                                          : tr("SteamCMD ended without downloading it (code %1): %2").arg(code).arg(m_lastLine);
    }
    setActivityIdle();
    notify();
    runNextJob();
}

void SteamCmdManager::cancel(const QString &itemId)
{
    bool changedAny = false;
    for (SteamCmdJob &job : m_jobs) {
        if (!itemId.isEmpty() && job.itemId != itemId)
            continue;
        if (job.state == SteamCmdJob::Queued) {
            job.state = SteamCmdJob::Failed;
            job.error = tr("Cancelled.");
            changedAny = true;
        }
    }
    if (SteamCmdJob *job = runningJob(); job && (itemId.isEmpty() || job->itemId == itemId)) {
        failCurrentJob(tr("Cancelled."));
        return;
    }
    if (changedAny)
        notify();
}

} // namespace murasaki
