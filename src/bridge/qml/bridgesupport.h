#pragma once

#include <functional>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <chrono>
#include <deque>
#include <optional>

namespace murasaki {

// Bridge Scaling mode (0 fill, 1 fit, 2 stretch); unknown names fall back to fill.
unsigned bridgeScalingMode(const QString &fill);

// Property helpers shared by the bridge consumers. Numeric means an integer or floating point QMetaType.
bool bridgeIsNumeric(QMetaType::Type type);
bool bridgeIsFloating(QMetaType::Type type);
// Keys travel as `key=value` on a command line, so '=' cannot appear in one; an empty key or a leading '-'
// would be read as an option.
bool bridgePropertyKeyUsable(const QString &key);

// The properties as a bridge Properties JSON object (bool, number, string, array of numbers). Entries with
// an unsupported value or an unusable key are skipped. `only` limits it to those keys; an
// empty list means all keys.
QJsonObject bridgePropertiesJson(const QVariantMap &properties, const QStringList &only = {});

// Restart policy for a crashing engine. Delays grow 1 s, 2 s, 4 s (the last one repeats); the engine is given up
// on when `maxCrashes` crashes happen within `window`. Time is passed in so the policy is testable.
class RestartBackoff
{
public:
    using Millis = std::chrono::milliseconds;

    explicit RestartBackoff(int maxCrashes = 3, Millis window = std::chrono::seconds(60));

    // Records a crash at `now` (any monotonic clock). Returns how long to wait before restarting, or nullopt
    // when the engine keeps crashing and should be given up on.
    std::optional<Millis> recordCrash(Millis now);
    void reset();
    int recentCrashes() const { return static_cast<int>(m_crashes.size()); }

private:
    int m_maxCrashes;
    Millis m_window;
    std::deque<Millis> m_crashes;
};

// Notices that the program a host runs was replaced on disk (an install), so that the item can start the new one: a child keeps running the old binary until it is
// restarted, and in plasmashell nobody else would. watch() records what the file is now (device, inode, size, modification time: a rename over it changes the inode);
// the directory is watched too (a replacement by rename loses the watch on the file), and a slow poll catches what no event told. A change is reported once the file is
// there, executable and the same for a whole quiet period (an install writes several files, and a binary that is half written must not be started), never before.
class ProgramWatch : public QObject
{
    Q_OBJECT

public:
    struct Signature {
        bool valid = false;
        quint64 device = 0;
        quint64 inode = 0;
        qint64 size = 0;
        qint64 modifiedNs = 0;
        bool operator==(const Signature &) const = default;
    };

    explicit ProgramWatch(int quietMs = 2000, int pollMs = 60000, QObject *parent = nullptr);

    // Starts watching `program` (a path, or a name looked up in PATH) and takes its present state as the one that runs. Empty: stops.
    void watch(const QString &program);
    QString path() const { return m_path; }
    // The signature of an executable file (following links); invalid when it is missing or not executable.
    static Signature signatureOf(const QString &path);

signals:
    // The program on disk is no longer the one that was watch()ed, and is stable now.
    void changed(const QString &path);

private:
    void touched();
    void settle();
    void arm();

    int m_quietMs;
    QString m_path;
    Signature m_known;
    Signature m_pending;
    QFileSystemWatcher m_watcher;
    QTimer m_quiet;
    QTimer m_poll;
};

// Host processes that are being stopped but are not gone yet, for the hosts of which only one may live at a time (a puppet engine). A stopped host is retired, not waited for: it is asked to end (the caller has sent Quit and closed
// the socket), the application takes the process over - it outlives the item that started it, so a window that is destroyed does not block, and a new window cannot lose track of it - and a timer kills it if it is still there after
// `killAfterMs`. anyLeaving() tells whether one is still alive; a new host of that kind waits until it is not, which is the whole guarantee: at most one alive.
namespace HostRetirement {
void retire(QProcess *process, int killAfterMs, std::function<void(int, QProcess::ExitStatus)> onEnded = {});
bool anyLeaving();
} // namespace HostRetirement

} // namespace murasaki
