#include "bridgesupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <algorithm>

#include <QJsonArray>

#include <cmath>

#include <sys/stat.h>

namespace murasaki {

bool bridgeIsNumeric(QMetaType::Type type)
{
    switch (type) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Float:
    case QMetaType::Double:
        return true;
    default:
        return false;
    }
}

bool bridgeIsFloating(QMetaType::Type type)
{
    return type == QMetaType::Float || type == QMetaType::Double;
}

bool bridgePropertyKeyUsable(const QString &key)
{
    return !key.isEmpty() && !key.contains(QLatin1Char('=')) && !key.startsWith(QLatin1Char('-'));
}

unsigned bridgeScalingMode(const QString &fill)
{
    if (fill == QLatin1String("fit"))
        return 1;
    if (fill == QLatin1String("stretch"))
        return 2;
    return 0;
}

QJsonObject bridgePropertiesJson(const QVariantMap &properties, const QStringList &only)
{
    QJsonObject out;
    for (auto it = properties.cbegin(); it != properties.cend(); ++it) {
        if (!bridgePropertyKeyUsable(it.key()) || (!only.isEmpty() && !only.contains(it.key())))
            continue;
        const QVariant &value = it.value();
        const auto type = static_cast<QMetaType::Type>(value.typeId());
        if (type == QMetaType::Bool) {
            out.insert(it.key(), value.toBool());
        } else if (type == QMetaType::QString) {
            out.insert(it.key(), value.toString());
        } else if (bridgeIsNumeric(type)) {
            out.insert(it.key(), value.toDouble());
        } else if (type == QMetaType::QVariantList) {
            QJsonArray array;
            bool ok = !value.toList().isEmpty();
            for (const QVariant &component : value.toList()) {
                ok = ok && bridgeIsNumeric(static_cast<QMetaType::Type>(component.typeId()));
                array.append(component.toDouble());
            }
            if (ok)
                out.insert(it.key(), array);
        }
    }
    return out;
}

RestartBackoff::RestartBackoff(int maxCrashes, Millis window)
    : m_maxCrashes(std::max(1, maxCrashes))
    , m_window(window)
{
}

std::optional<RestartBackoff::Millis> RestartBackoff::recordCrash(Millis now)
{
    while (!m_crashes.empty() && now - m_crashes.front() >= m_window)
        m_crashes.pop_front();
    m_crashes.push_back(now);
    const int n = static_cast<int>(m_crashes.size());
    if (n >= m_maxCrashes)
        return std::nullopt;
    return Millis(1000 << std::min(n - 1, 2));
}

void RestartBackoff::reset()
{
    m_crashes.clear();
}

namespace HostRetirement {

namespace {

QList<QPointer<QProcess>> &leaving()
{
    static QList<QPointer<QProcess>> list;
    return list;
}

} // namespace

void retire(QProcess *process, int killAfterMs, std::function<void(int, QProcess::ExitStatus)> onEnded)
{
    if (!process)
        return;
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    if (QCoreApplication::instance())
        process->setParent(QCoreApplication::instance());
    leaving().append(process);
    if (onEnded) {
        // (an end the retirement itself caused by its kill is not the host's: only one it came to on its own is told)
        QObject::connect(process, &QProcess::finished, process, [process, onEnded = std::move(onEnded)](int code, QProcess::ExitStatus status) {
            if (!process->property("murasakiRetireKilled").toBool())
                onEnded(code, status);
        });
    }
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            process->deleteLater();
    });
    // (the output of a host on its way out is read and thrown away, or it could block on a full pipe and never end)
    QObject::connect(process, &QProcess::readyRead, process, [process] { process->readAll(); });
    QTimer::singleShot(std::max(0, killAfterMs), process, [process] {
        if (process->state() != QProcess::NotRunning) {
            process->setProperty("murasakiRetireKilled", true);
            process->kill();
        }
    });
}

bool anyLeaving()
{
    auto &list = leaving();
    for (auto it = list.begin(); it != list.end();) {
        if (!*it || (*it)->state() == QProcess::NotRunning)
            it = list.erase(it);
        else
            return true;
    }
    return false;
}

} // namespace HostRetirement

// ---- ProgramWatch ------------------------------------------------------------------------------------------

ProgramWatch::ProgramWatch(int quietMs, int pollMs, QObject *parent)
    : QObject(parent)
    , m_quietMs(quietMs)
{
    m_quiet.setSingleShot(true);
    connect(&m_quiet, &QTimer::timeout, this, &ProgramWatch::settle);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &ProgramWatch::touched);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &ProgramWatch::touched);
    m_poll.setInterval(pollMs);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (!m_quiet.isActive() && signatureOf(m_path) != m_known)
            touched(); // (an event that never came)
    });
}

ProgramWatch::Signature ProgramWatch::signatureOf(const QString &path)
{
    struct stat st {};
    if (path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & S_IXUSR) == 0)
        return {};
    return {true, quint64(st.st_dev), quint64(st.st_ino), qint64(st.st_size), qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec};
}

void ProgramWatch::watch(const QString &program)
{
    m_quiet.stop();
    m_poll.stop();
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_path = program.isEmpty() || program.contains(u'/') ? program : QStandardPaths::findExecutable(program);
    m_known = m_pending = signatureOf(m_path);
    if (m_path.isEmpty())
        return;
    arm();
    m_poll.start();
}

// The file and its directory (and the directory of the target when it is a link): the watch on a file is lost when it is replaced by a rename.
void ProgramWatch::arm()
{
    const QFileInfo info(m_path);
    QStringList directories{info.absolutePath()};
    if (const QString real = info.canonicalFilePath(); !real.isEmpty())
        directories << QFileInfo(real).absolutePath();
    directories.removeDuplicates();
    for (const QString &directory : std::as_const(directories))
        if (!m_watcher.directories().contains(directory))
            m_watcher.addPath(directory);
    for (const QString &file : {m_path, info.canonicalFilePath()})
        if (!file.isEmpty() && !m_watcher.files().contains(file))
            m_watcher.addPath(file);
}

void ProgramWatch::touched()
{
    if (m_path.isEmpty())
        return;
    m_quiet.start(m_quietMs); // (every event of the install pushes the end of the quiet period)
}

void ProgramWatch::settle()
{
    arm(); // (the replacement is a new file: watched again)
    const Signature now = signatureOf(m_path);
    if (now == m_known) {
        m_pending = now;
        return;
    }
    // Not executable yet or not the same as a quiet period ago: the install is still going. Looked at again; the poll keeps looking if no event comes.
    if (!now.valid) {
        m_pending = now; // (missing: the directory's next event or the slow poll looks again)
        return;
    }
    if (now != m_pending) {
        m_pending = now;
        m_quiet.start(m_quietMs);
        return;
    }
    m_known = now;
    emit changed(m_path);
}

} // namespace murasaki
