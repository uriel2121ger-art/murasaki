#include "filelog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <QStandardPaths>

#include <memory>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

QtMessageHandler g_previousHandler = nullptr;
RotatingLog *g_processLog = nullptr;

QChar levelLetter(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return u'D';
    case QtInfoMsg:
        return u'I';
    case QtWarningMsg:
        return u'W';
    case QtCriticalMsg:
        return u'E';
    case QtFatalMsg:
        return u'F';
    }
    return u'?';
}

QString stamp() { return QDateTime::currentDateTime().toString(Qt::ISODateWithMs); }

void fileHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_processLog) {
        const QString category = context.category && QLatin1String(context.category) != "default" ? QString::fromLatin1(context.category) + u": "_s
                                                                                               : QString();
        g_processLog->append(u"%1 [%2] %3%4"_s.arg(stamp(), QString(levelLetter(type)), category, message));
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

} // namespace

QString logDirectory()
{
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty() || !QDir::isAbsolutePath(state)) // a relative one is invalid (XDG base directory spec)
        state = QDir::homePath() + u"/.local/state"_s;
    return state + u"/murasaki/logs"_s;
}

QString logFileName(const QString &component, const QString &output)
{
    QString name = output.isEmpty() ? component : component + u'-' + output;
    for (QChar &c : name)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != u'.' && c != u'_' && c != u'-')
            c = u'_';
    if (name.isEmpty() || name.startsWith(u'.'))
        name.prepend(u'_');
    return name + u".log"_s;
}

RotatingLog::RotatingLog(QString path, qint64 maxBytes, int files)
    : m_path(std::move(path))
    , m_maxBytes(maxBytes)
    , m_files(files < 1 ? 1 : files)
{
}

RotatingLog::~RotatingLog() = default;

bool RotatingLog::open()
{
    if (m_file.isOpen())
        return true;
    const QFileInfo info(m_path);
    QDir().mkpath(info.absolutePath());
    QFile::setPermissions(info.absolutePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void RotatingLog::rotate()
{
    m_file.close();
    QFile::remove(m_path + u'.' + QString::number(m_files - 1));
    for (int i = m_files - 2; i >= 1; --i)
        QFile::rename(m_path + u'.' + QString::number(i), m_path + u'.' + QString::number(i + 1));
    if (m_files > 1)
        QFile::rename(m_path, m_path + u".1"_s);
    else
        QFile::remove(m_path);
}

void RotatingLog::append(const QString &line)
{
    const QByteArray bytes = line.toUtf8() + '\n';
    QMutexLocker lock(&m_mutex);
    if (!open())
        return;
    if (m_file.size() > 0 && m_file.size() + bytes.size() > m_maxBytes) {
        rotate();
        if (!open())
            return;
    }
    m_file.write(bytes);
    m_file.flush();
}

void installFileLogging(const QString &component)
{
    if (g_processLog)
        return;
    g_processLog = new RotatingLog(logDirectory() + u'/' + logFileName(component)); // lives as long as the process
    g_previousHandler = qInstallMessageHandler(fileHandler);
}

void appendChildLog(const QString &name, const QString &line)
{
    static QMutex mutex;
    static QHash<QString, RotatingLog *> logs;
    RotatingLog *log = nullptr;
    {
        QMutexLocker lock(&mutex);
        RotatingLog *&slot = logs[name];
        if (!slot)
            slot = new RotatingLog(logDirectory() + u'/' + logFileName(name)); // never freed: they live as long as the process
        log = slot;
    }
    log->append(stamp() + u' ' + line);
}

} // namespace murasaki
