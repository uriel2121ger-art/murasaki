#include "autostart.h"

#include <QElapsedTimer>
#include <QProcess>

#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki::autostart {
namespace {

constexpr int kQueryTimeoutMs = 5000;
constexpr int kStartTimeoutMs = 5000;

CommandRunner &runnerStorage()
{
    static CommandRunner runner;
    return runner;
}

int runSystemctl(const QStringList &arguments, int timeoutMs, bool forwardOutput)
{
    if (const CommandRunner &custom = runnerStorage())
        return custom(arguments, timeoutMs, forwardOutput);
    QProcess process;
    if (forwardOutput) {
        process.setProcessChannelMode(QProcess::ForwardedChannels);
    } else {
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
    }
    QElapsedTimer clock;
    clock.start();
    process.start(u"systemctl"_s, arguments);
    if (!process.waitForStarted(timeoutMs < 0 ? kStartTimeoutMs : timeoutMs))
        return -1;
    const int remaining = timeoutMs < 0 ? -1 : qMax<qint64>(0, timeoutMs - clock.elapsed());
    if (!process.waitForFinished(remaining)) {
        process.kill();
        process.waitForFinished();
        return -1;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

} // namespace

CommandRunner setCommandRunner(CommandRunner runner)
{
    return std::exchange(runnerStorage(), std::move(runner));
}

QStringList queryEnabledArguments()
{
    return {u"--user"_s, u"is-enabled"_s, u"--quiet"_s, kSurfaceUnit};
}

QString changeEnabledVerb(bool enabled)
{
    return enabled ? u"enable"_s : u"disable"_s;
}

QStringList changeEnabledArguments(bool enabled)
{
    return {u"--user"_s, changeEnabledVerb(enabled), u"--now"_s, kSurfaceUnit};
}

QStringList startArguments()
{
    return {u"--user"_s, u"start"_s, kSurfaceUnit};
}

bool isEnabled()
{
    return runSystemctl(queryEnabledArguments(), kQueryTimeoutMs, false) == 0;
}

bool setEnabled(bool enabled, QString *error, bool *couldNotRun)
{
    const QString verb = changeEnabledVerb(enabled);
    const int code = runSystemctl(changeEnabledArguments(enabled), -1, true);
    if (couldNotRun)
        *couldNotRun = code < 0;
    if (code == 0)
        return true;
    if (error)
        *error = code < 0 ? u"cannot run systemctl"_s : u"systemctl %1 failed (exit code %2)"_s.arg(verb).arg(code);
    return false;
}

} // namespace murasaki::autostart
