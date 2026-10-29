#pragma once

#include <QFile>
#include <QMutex>
#include <QString>
#include <QtGlobal>

namespace murasaki {

inline constexpr qint64 kLogFileBytes = 2 * 1024 * 1024; // size at which a log file is rotated
inline constexpr int kLogFileCount = 3;                  // the current file plus the older ones kept (.1, .2)

// Where the logs live: $XDG_STATE_HOME/murasaki/logs (default ~/.local/state/murasaki/logs). Not created here.
QString logDirectory();

// "<component>.log", or "<component>-<output>.log" when there is an output. Anything but letters, digits, '.', '_' and
// '-' becomes '_' and a leading '.' is replaced, so a name from outside can never leave the directory.
QString logFileName(const QString &component, const QString &output = QString());

// An append-only text file that rotates by size: when a line would take it past maxBytes, name becomes name.1, name.1
// becomes name.2, and so on up to `files` files in all (the oldest is deleted). The file and its directory are private
// to the user (0600, 0700): logs name items and paths. Thread-safe. A log that cannot be written is silently dropped:
// logging must never take the program down.
class RotatingLog
{
public:
    explicit RotatingLog(QString path, qint64 maxBytes = kLogFileBytes, int files = kLogFileCount);
    ~RotatingLog();
    RotatingLog(const RotatingLog &) = delete;
    RotatingLog &operator=(const RotatingLog &) = delete;

    void append(const QString &line); // one line, a newline is added
    QString path() const { return m_path; }

private:
    bool open();
    void rotate();

    QString m_path;
    qint64 m_maxBytes;
    int m_files;
    QMutex m_mutex;
    QFile m_file;
};

// Sends everything this process prints with qDebug/qInfo/qWarning/qCritical to <logDirectory>/<component>.log as well as
// to the previous handler (stderr, the journal). Call once, right after the QCoreApplication exists. A process whose
// stderr is captured by its parent (a Plasma host) must not call it: the parent writes those lines.
void installFileLogging(const QString &component);

// One line from a child process (an engine's stderr) into <logDirectory>/<name>.log, timestamped. The logs are opened
// on first use and kept for the life of the process.
void appendChildLog(const QString &name, const QString &line);

} // namespace murasaki
