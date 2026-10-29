#pragma once

#include "petstore.h"
#include "transcodebuckets.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>

namespace murasaki {

inline constexpr double kTranscodeHalveFpsFrom = 50;

// One pet variant to transcode. Everything comes from the library item; the manager never opens the source for
// writing.
struct TranscodeSource {
    QString itemId;
    QString variant; // PetVariant::name
    QString file;    // absolute path of the original
    double fps = 0;
    int frames = 0;
    int bucket = kTranscodeMaxSide; // longest side to convert to, one of kTranscodeBuckets
    // The rate the copy steps its frames at: every source frame is kept and shown for 1/playFps seconds, as Anima
    // Engine does (it ignores the delays of the file). 0 keeps the file's own timing.
    int playFps = 0;
};

struct TranscodeStatus {
    enum State { None, Queued, Running, Done, Failed };
    State state = None;
    int bucket = 0;      // the bucket this is the state of (0 when unknown)
    int playFps = 0;     // the playback rate of the copy (0: the file's own timing)
    double progress = 0; // 0..1, while Running
    QString file;        // the cached webm, when Done
    QString error;       // the last ffmpeg stderr lines (or the reason), when Failed

    static QString stateName(State state);
    QJsonObject toJson() const;
};

// Converts heavy pet animations to VP9 with alpha (longest side one of kTranscodeBuckets, 30 fps for 50+ fps
// sources) in <cacheRoot>/<item>/<variant>-<bucket>-<key>.webm, one ffmpeg at a time under nice. The cache key is a hash of the source
// path, mtime and size, so an edited source is converted again. Output is written to a ".part" file in the cache
// dir and renamed when ffmpeg succeeds; nothing outside cacheRoot is ever written.
class TranscodeManager : public QObject
{
    Q_OBJECT

public:
    explicit TranscodeManager(QString cacheRoot, QObject *parent = nullptr);
    ~TranscodeManager() override;

    QString cacheRoot() const { return m_root; }

    // How long ffmpeg may stay silent (it reports progress twice a second) before it is killed.
    void setStallTimeoutMs(int ms) { m_stallMs = ms; }
    // The most a conversion to a stage's bucket (above kTranscodeMaxSide) may take in all, 30 minutes by default: past it the job is killed and fails, and the
    // original plays. (The stall timeout only catches a job that stopped; a 4K encode that crawls would hold the queue.)
    void setStageBudgetMs(int ms) { m_stageBudgetMs = ms; }

    TranscodeStatus status(const TranscodeSource &source) const;
    // Where the transcoded copy of source lives (whether or not it exists yet).
    QString outputPath(const TranscodeSource &source) const;

    // Queues the variant unless it is already done, queued or running. A failed one is tried again only when
    // retryFailed is set (an explicit request of the user); automatic requests (a pet was added or edited) leave
    // it failed, so a broken file is not converted over and over. Returns false when the source is not a readable
    // file. Emits changed(itemId).
    bool request(const TranscodeSource &source, bool retryFailed = true);

    // Startup garbage collection. expected lists every conversion (variant and bucket) to keep; libraryItems the
    // ids of all pet items of the library. A file of one of those items that is not in expected is removed (a copy
    // from before buckets existed too), and so is a leftover .part. Dirs of items that
    // are no longer in the library are only cleaned of files older than maxAgeDays, so an unplugged Steam drive
    // does not cost the whole cache.
    void pruneCache(const QList<TranscodeSource> &expected, const QStringList &libraryItems = {}, int maxAgeDays = 7);

    // The buckets of this variant that are converted at source.playFps (for the current state of the source file),
    // newest first.
    QList<int> doneBuckets(const TranscodeSource &source) const;
    // A bucket and playback rate that have a copy.
    struct DoneCopy {
        int bucket = 0;
        int playFps = 0;
    };
    // Every copy of this variant, whatever its rate, newest first.
    QList<DoneCopy> doneCopies(const TranscodeSource &source) const;
    // The state of every bucket and rate of this variant that is not "none": finished copies, the one running,
    // queued ones and failures. source only names the item, the variant and the file.
    QList<TranscodeStatus> statuses(const TranscodeSource &source) const;
    // The conversions waiting for their turn (not the running one), in order.
    QList<TranscodeSource> queued() const;
    // Drops source from the queue; a running or finished one is left alone. Emits changed(itemId) when dropped.
    bool cancelQueued(const TranscodeSource &source);
    // Deletes the converted copy of source.bucket. Only a finished one: a queued or running bucket is left alone.
    // Emits changed(itemId) when a file was removed.
    bool removeDone(const TranscodeSource &source);

    // ffmpeg's -vf expression for a source of this frame rate, converted to this longest side. playFps > 0 keeps
    // every frame and spaces them 1/playFps apart (the file's delays are dropped); otherwise sources of 50 fps or
    // more are halved to 30.
    static QString filterFor(double fps, int bucket = kTranscodeMaxSide, int playFps = 0);
    static QStringList ffmpegArguments(const TranscodeSource &source, const QString &outputPart);

signals:
    // The status of some variant of itemId changed (queued, progress of at least 2 points, done, failed).
    void changed(const QString &itemId);

private:
    struct Job {
        TranscodeSource source;
        QString output;
    };

    static QString key(const TranscodeSource &source);
    // Where copies made before the playback rate existed lived: <variant>-<bucket>-<key>.webm, and before buckets
    // <variant>-<key>.webm (the biggest bucket). They are copies at the file's own timing, and count as such while
    // their key matches the source, until a new conversion replaces them.
    QStringList legacyPaths(const TranscodeSource &source) const;
    struct ParsedName {
        int bucket = 0;
        int playFps = 0;
        QString key;
        bool part = false;
        bool legacy = false;
    };
    // The bucket, rate and key in a cache file name (after the variant prefix); nothing when it is not one.
    static std::optional<ParsedName> parseName(const QString &rest);
    static QString variantPrefix(const QString &variant); // "<safe name>-<hash of the real name>-"
    static QString safeName(const QString &name);
    QString dirFor(const QString &itemId) const;
    void startNext();
    void finish(const QString &error);
    void cleanStale(const Job &job) const;
    void onStdout();
    void onStderr();

    QString m_root;
    int m_stallMs = 5 * 60 * 1000;
    int m_stageBudgetMs = 30 * 60 * 1000;
    QTimer m_budget;
    bool m_killedByBudget = false;
    QList<Job> m_queue;
    std::unique_ptr<QProcess> m_process;
    Job m_current;
    bool m_hasCurrent = false;
    double m_progress = 0;
    double m_reportedProgress = 0;
    QByteArray m_outBuffer;
    QByteArray m_errBuffer;
    QStringList m_errLines;
    QHash<QString, QString> m_failures; // output path -> reason
    QTimer m_watchdog;
    bool m_killedByWatchdog = false;
};

} // namespace murasaki
