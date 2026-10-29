#include "sandboxreport.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>

namespace murasaki {

QString SandboxReport::summary() const
{
    if (!valid)
        return QStringLiteral("not available (murasaki-sandbox is missing or does not run)");
    const QString landlock = landlockAbi >= 1 ? QStringLiteral("Landlock ABI %1").arg(landlockAbi) : QStringLiteral("no Landlock");
    return QStringLiteral("%1, TCP %2, mount namespace %3")
        .arg(landlock, tcpDeny ? QStringLiteral("refused") : QStringLiteral("not refused"),
             bwrap ? QStringLiteral("yes") : QStringLiteral("no"));
}

SandboxReport parseSandboxReport(const QByteArray &output)
{
    SandboxReport report;
    const QJsonObject object = QJsonDocument::fromJson(output.trimmed()).object();
    if (!object.value(QStringLiteral("landlockAbi")).isDouble())
        return report;
    report.landlockAbi = qBound(0, object.value(QStringLiteral("landlockAbi")).toInt(), 100);
    report.tcpDeny = object.value(QStringLiteral("tcpDeny")).toBool();
    report.scopes = object.value(QStringLiteral("scopes")).toBool();
    report.bwrap = object.value(QStringLiteral("bwrap")).toBool();
    report.valid = true;
    return report;
}

SandboxReport querySandboxReport(const QString &helper)
{
    if (helper.isEmpty())
        return {};
    static QMutex lock;
    static QHash<QString, SandboxReport> cache;
    const QFileInfo info(helper);
    const QString key = helper + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
        + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        const QMutexLocker locker(&lock);
        const auto it = cache.constFind(key);
        if (it != cache.cend())
            return *it;
    }
    QProcess process;
    process.start(helper, {QStringLiteral("--report")});
    SandboxReport report;
    if (process.waitForFinished(15000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        report = parseSandboxReport(process.readAllStandardOutput());
    else
        process.kill();
    const QMutexLocker locker(&lock);
    cache.insert(key, report);
    return report;
}

} // namespace murasaki
