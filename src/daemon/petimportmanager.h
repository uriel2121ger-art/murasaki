#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

namespace murasaki {

// Runs pet imports (docs/pets/import.md, "Daemon"), one at a time, in the helper murasaki-petimport behind murasaki-sandbox. The manager validates the spec
// with the same function the helper uses, writes it under <outRoot>/.specs, starts the helper and relays its JSON lines (progress at most four a second).
// The result lands in <outRoot>/<id>/; adding it to the local pets is the service's job (done() says when). Nothing here decodes a file.
class PetImportManager : public QObject
{
    Q_OBJECT

public:
    enum class State { Queued, Running, Done, Failed, Cancelled };

    explicit PetImportManager(QString outRoot, QObject *parent = nullptr);
    ~PetImportManager() override;

    QString outRoot() const { return m_outRoot; }
    // The helper to run; `sandbox` is murasaki-sandbox (empty: none). With requireSandbox a job fails with "sandbox unavailable" when there is none (or
    // when the sandbox itself cannot confine, which it reports with status 125).
    void setHelper(QString helper, QString sandbox, bool requireSandbox);
    void setMaxOutputBytes(long long bytes) { m_maxOutput = bytes; }
    void setWallClockMs(int ms) { m_wallMs = ms; }

    // Queues a job. The job id, or an empty string with *error set (a name of petimport.h). At most kMaxQueued unfinished jobs.
    QString submit(const QByteArray &specJson, QString *error);
    // Header-only look at a spec, in the helper too (it waits at most eight seconds): {"ok":true,"preview":{...}} or {"ok":false,"error":"..."}; an empty
    // string with *error set when the spec itself is not valid.
    QString preview(const QByteArray &specJson, QString *error);
    // The same without waiting: `done` gets the answer text later (a D-Bus reply sent when the helper has spoken). At most one preview runs at a time, up to
    // kMaxPendingPreviews wait, and the same spec asked while it is pending shares the answer; beyond that the answer is {"ok":false,"error":"busy"}.
    // False with *error set when the spec itself is not valid (nothing is queued).
    bool previewAsync(const QByteArray &specJson, std::function<void(const QString &)> done, QString *error);
    // Queued jobs are dropped; a running one is stopped (SIGTERM, then SIGKILL after five seconds) and its work folder removed by the helper.
    bool cancel(const QString &jobId);

    QJsonArray jobs() const;
    QJsonObject job(const QString &jobId) const;
    // The service records the item that was made from a finished job.
    void setItemId(const QString &jobId, const QString &itemId);
    // The service could not turn a finished job into a pet: the job is shown as failed with the reason.
    void markFailed(const QString &jobId, const QString &error);
    // Removes the work folders and spec files a crash left (call before the first job).
    void cleanStale();

    static constexpr int kMaxQueued = 20;
    static constexpr int kKeepFinished = 50;
    static constexpr int kMaxPendingPreviews = 8;

signals:
    void changed(const QString &jobId);
    // A job finished successfully; `file` is the animation to add as a local pet.
    void done(const QString &jobId, const QString &file);

private:
    struct Job {
        QString id;
        QString kind;
        QString name;
        QStringList sources;
        QByteArray specJson;
        State state = State::Queued;
        double progress = 0;
        int frames = 0;
        int variants = 0;
        QString error;
        QStringList degraded;
        QString file;
        bool unconfined = false;
        QString itemId;
        bool cancelRequested = false;
    };

    Job *find(const QString &jobId);
    const Job *find(const QString &jobId) const;
    void startNext();
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void finish(Job &job, State state, const QString &error);
    void trim();
    QStringList commandFor(const Job &job, const QString &specPath, QString *program) const;

    QString m_outRoot;
    QString m_helper;
    QString m_sandbox;
    bool m_requireSandbox = false;
    long long m_maxOutput = 1LL << 30;
    int m_wallMs = 30 * 60 * 1000;
    QList<Job> m_jobs;
    std::unique_ptr<QProcess> m_process;
    QString m_runningId;
    QByteArray m_buffer;
    QByteArray m_stderr;
    QElapsedTimer m_lastEmit;
    bool m_sawDone = false;
    bool m_sawError = false;
    QTimer m_wallClock;
    QTimer m_killTimer;
    int m_counter = 0;
    bool m_timedOut = false;

    struct PendingPreview {
        QByteArray spec;
        QList<std::function<void(const QString &)>> waiting;
    };
    QList<PendingPreview> m_previews;
    std::unique_ptr<QProcess> m_previewProcess;
    QString m_previewSpecFile;
    QTimer m_previewTimer;
    void startNextPreview();
    void finishPreview(const QString &answer);
};

} // namespace murasaki
