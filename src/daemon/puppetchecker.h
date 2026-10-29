#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>

#include <deque>
#include <functional>
#include <memory>

namespace murasaki {

// The deeper check of a puppet folder (docs/pets/puppets.md section 11): `murasaki-puppet --check FOLDER`, which reads the whole puppet of an `.inp` and counts what a Cubism manifest names, run behind
// `murasaki-sandbox --profile puppet` (the helper that reads hostile models never runs in the daemon). One at a time, with a deadline; the answer (the helper's one line of JSON) is kept in memory,
// keyed by the folder and a signature of its files (the names, sizes and times of its top level), so that an unchanged folder is not checked again and a changed one is. `changed(folder)` says an answer arrived.
// A check that cannot run (no helper, the sandbox required and missing, the deadline, a crash, a line that is not JSON) answers {"ok": false, "reasons": ["puppet: check failed: <why>"]}: the item shows as
// invalid with that reason instead of waiting for ever.
class PuppetChecker : public QObject
{
    Q_OBJECT

public:
    enum class Sandbox { Off, Prefer, Require };
    static constexpr int kDeadlineMs = 30000;
    static constexpr int kMaxQueued = 64;
    static constexpr int kMaxOutputBytes = 64 * 1024; // the most of its answer that is read

    // `helper` and `sandbox`: executable paths; empty to look them up. `mode` is asked when a check starts.
    PuppetChecker(QString helper, QString sandbox, QObject *parent = nullptr);
    ~PuppetChecker() override;
    void setSandboxMode(std::function<Sandbox()> mode) { m_mode = std::move(mode); }
    void setDeadlineMs(int ms) { m_deadlineMs = ms; } // (the tests)

    // The answer for the folder as it is now (empty object: none yet). Starts a check when there is none for this signature and none is queued.
    QJsonObject result(const QString &folder);
    // Forget the answer for the folder (it was removed).
    void forget(const QString &folder);

    // The command line of a check: the program, then its arguments (the sandbox, when there is one, in front). Static for the tests.
    static QStringList command(const QString &helper, const QString &sandbox, bool require, const QString &folder);
    static QString signatureOf(const QString &folder);

signals:
    void changed(const QString &folder);

private:
    struct Entry {
        QString signature;
        QJsonObject answer;
        bool running = false;
    };
    void startNext();
    void complete(const QJsonObject &answer);
    void finish(const QString &folder, const QString &signature, const QJsonObject &answer);

    QString m_helper, m_sandbox;
    std::function<Sandbox()> m_mode;
    QHash<QString, Entry> m_entries;
    std::deque<QString> m_queue;
    std::unique_ptr<QProcess> m_process;
    QString m_current;
    QString m_currentSignature; // the folder's signature when this check started
    QByteArray m_output;
    bool m_timedOut = false;
    bool m_tooLong = false;
    int m_deadlineMs = kDeadlineMs;
};

} // namespace murasaki
