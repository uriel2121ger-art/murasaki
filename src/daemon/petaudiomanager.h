#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <deque>
#include <functional>
#include <memory>

namespace murasaki {

// The sounds of the desktop pets (docs/pets/audio.md): turns a file the user picked into a normalised asset by running `murasaki-petaudio --import`
// (the one place a codec touches hostile audio) behind murasaki-sandbox when it is available, one job at a time, and keeps the asset folder.
class PetAudioManager : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxQueued = 8;
    static constexpr int kKeptJobs = 16;
    static constexpr long long kDeadlineMs = 5LL * 60 * 1000;

    // `assetsDir` is <data>/murasaki/pets/audio. `helper` and `sandbox` are executable paths, empty to look them up beside the daemon and in PATH.
    // `sandboxMode` says whether the sandbox may be skipped when missing: it is asked when a job starts.
    enum class Sandbox { Off, Prefer, Require };
    PetAudioManager(QString assetsDir, QString helper, QString sandbox, QObject *parent = nullptr);

    ~PetAudioManager() override;
    QString assetsDir() const { return m_assetsDir; }
    void setSandboxMode(std::function<Sandbox()> mode) { m_sandboxMode = std::move(mode); }

    // Queues the import of `path` (an absolute path of a regular file). Empty id with *error set when refused (not absolute, queue full).
    QString importFile(const QString &path, QString *error);
    // {"id", "state": queued|running|done|failed|cancelled, "name", "error"?, "result"?} or an empty object for an unknown id.
    QJsonObject job(const QString &id) const;
    bool cancel(const QString &id);

    // [{"asset", "name", "durationMs", "sourceFormat", "bytes", "refs"}] for the assets in the folder; `referenced` lists the asset id of each use.
    QJsonArray assets(const QStringList &referenced) const;
    // Refused (false, *error set) while `referenced` lists the asset; removes the .wav and the .json.
    bool removeAsset(const QString &asset, const QStringList &referenced, QString *error);
    // Removes the assets nothing references and that are older than `minAgeSeconds`; returns how many.
    int collect(const QStringList &referenced, long long minAgeSeconds);

signals:
    void changed(const QString &jobId); // a job changed state, or (jobId empty) the assets did

private:
    struct Job {
        QString id;
        QString path;
        QString name;
        QString state = QStringLiteral("queued");
        QString error;
        QJsonObject result;
    };
    void startNext();
    void finished(int exitCode, QProcess::ExitStatus status);
    Job *find(const QString &id);
    const Job *find(const QString &id) const;
    void trim();

    QString m_assetsDir;
    QString m_helper;
    QString m_sandbox;
    std::function<Sandbox()> m_sandboxMode;
    std::deque<Job> m_jobs; // oldest first; at most kKeptJobs
    std::unique_ptr<QProcess> m_process;
    QString m_running;
    QString m_output;
    int m_counter = 0;
};

} // namespace murasaki
