#pragma once

#include <QHash>
#include <QString>

namespace murasaki {

// Remembers which files took the overlay down. libmpv decodes inside the overlay process, so a poison file does
// not fail politely: it kills the process, systemd restarts it, and every pet that shares it dies again. Before a
// file is handed to a window the ledger writes a small marker for that pet (atomically); the marker goes away when
// the file plays, fails cleanly or the window is closed. A marker found at startup is therefore a crash while that
// file was opening: it is added to the persistent count of the file, and after kMaxCrashes the file is blocked
// across restarts. A count belongs to the file's modification time and size, so replacing the file resets it.
//
// State lives in <dir> (the overlay's $XDG_STATE_HOME/murasaki/overlay): crashes.json and opening-<hash>.json.
class CrashLedger
{
public:
    static constexpr int kMaxCrashes = 3;
    static constexpr int kMaxFiles = 64; // the oldest records go first
    static constexpr qint64 kCrashForgetSeconds = 24 * 60 * 60; // (kCrashForgetHours of puppetlimits.h)

    explicit CrashLedger(QString dir);

    // Reads crashes.json and turns every leftover marker into a crash of its file. Call once, before any window.
    void load();

    int crashes(const QString &file) const;
    bool blocked(const QString &file) const { return crashes(file) >= kMaxCrashes; }

    // The pet's window is about to open file.
    void markOpening(const QString &petId, const QString &file);
    // The window no longer opens anything (it plays, failed cleanly, or is gone).
    void clearOpening(const QString &petId);
    // The file took a process down while it was shown (the puppet engine died on it): one more crash of it, counted now and kept across restarts like the ones found at startup. The count starts again
    // at one when the engine had been running a long time (`afterLongRun`, the host decides: kEngineLongRunSeconds) or the previous crash was over kCrashForgetHours ago: three crashes block a model
    // that breaks the engine, not one that ran for weeks and met a GPU reset, an out-of-memory kill and a driver update. `nowSecs` is the clock (seconds since the epoch; -1: the system's), for the tests.
    void recordCrash(const QString &file, bool afterLongRun = false, qint64 nowSecs = -1);
    // Forgets the file's crashes (the placement was removed, the user asked to try again, or it played).
    void forget(const QString &file);

private:
    struct Record {
        qint64 mtimeMs = 0;
        qint64 size = -1;
        int crashes = 0;
        qint64 at = 0; // seconds since the epoch, for the cap
    };

    QString markerPath(const QString &petId) const;
    static Record signatureOf(const QString &file);
    void save();

    QString m_dir;
    QHash<QString, Record> m_records;
    bool m_warnedWrite = false;
};

} // namespace murasaki
