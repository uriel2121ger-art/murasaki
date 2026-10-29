#include "transcodemanager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QPair>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {
constexpr int kKeptStderrLines = 8;
constexpr int kMaxLineBytes = 4096;
} // namespace

QString TranscodeStatus::stateName(State state)
{
    switch (state) {
    case None:
        return u"none"_s;
    case Queued:
        return u"queued"_s;
    case Running:
        return u"running"_s;
    case Done:
        return u"done"_s;
    case Failed:
        break;
    }
    return u"failed"_s;
}

QJsonObject TranscodeStatus::toJson() const
{
    QJsonObject json{{u"state"_s, stateName(state)}};
    if (bucket > 0) {
        json.insert(u"bucket"_s, bucket);
        json.insert(u"fps"_s, playFps);
    }
    if (state == Running)
        json.insert(u"progress"_s, progress);
    if (state == Done)
        json.insert(u"file"_s, file);
    if (state == Failed)
        json.insert(u"error"_s, error);
    return json;
}

TranscodeManager::TranscodeManager(QString cacheRoot, QObject *parent)
    : QObject(parent)
    , m_root(std::move(cacheRoot))
{
    m_watchdog.setSingleShot(true);
    m_budget.setSingleShot(true);
    connect(&m_budget, &QTimer::timeout, this, [this] {
        if (!m_process)
            return;
        m_killedByBudget = true;
        m_process->kill();
    });
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (!m_process)
            return;
        m_killedByWatchdog = true;
        m_process->kill();
    });
}

TranscodeManager::~TranscodeManager()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
        if (m_hasCurrent)
            QFile::remove(m_current.output + u".part"_s);
    }
}

QString TranscodeManager::safeName(const QString &name)
{
    static const QRegularExpression bad(u"[^A-Za-z0-9._-]"_s);
    QString safe = name;
    safe.replace(bad, u"_"_s);
    safe = safe.left(80);
    // A leading dot would hide the file and ".." would escape the dir.
    if (safe.isEmpty() || safe.startsWith(u'.'))
        safe.prepend(u'_');
    return safe;
}

QString TranscodeManager::variantPrefix(const QString &variant)
{
    // safeName() maps different names to the same text ("a b" and "a_b"); the hash keeps their files apart.
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(variant.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return safeName(variant) + u'-' + hash + u'-';
}

QString TranscodeManager::key(const TranscodeSource &source)
{
    const QFileInfo info(source.file);
    const QByteArray material = source.file.toUtf8() + '\n' + QByteArray::number(info.lastModified().toMSecsSinceEpoch())
        + '\n' + QByteArray::number(info.size());
    return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha1).toHex().left(12));
}

QString TranscodeManager::dirFor(const QString &itemId) const
{
    return m_root + u'/' + safeName(itemId);
}

QString TranscodeManager::outputPath(const TranscodeSource &source) const
{
    return dirFor(source.itemId) + u'/' + variantPrefix(source.variant) + QString::number(source.bucket) + u'-'
        + QString::number(source.playFps) + u'-' + key(source) + u".webm"_s;
}

QStringList TranscodeManager::legacyPaths(const TranscodeSource &source) const
{
    if (source.playFps != 0)
        return {};
    const QString base = dirFor(source.itemId) + u'/' + variantPrefix(source.variant);
    QStringList paths = {base + QString::number(source.bucket) + u'-' + key(source) + u".webm"_s};
    if (source.bucket == kTranscodeMaxSide)
        paths.append(base + key(source) + u".webm"_s);
    return paths;
}

std::optional<TranscodeManager::ParsedName> TranscodeManager::parseName(const QString &rest)
{
    // <bucket>-<fps>-<key>.webm, or the older <bucket>-<key>.webm and <key>.webm; a ".part" while converting.
    static const QRegularExpression named(u"\\A(?:([0-9]+)-([0-9]+)-|([0-9]+)-)?([0-9a-f]{12})\\.webm(\\.part)?\\z"_s);
    const QRegularExpressionMatch match = named.match(rest);
    if (!match.hasMatch())
        return std::nullopt;
    ParsedName parsed;
    parsed.key = match.captured(4);
    parsed.part = match.capturedLength(5) > 0;
    if (match.capturedLength(1) > 0) {
        parsed.bucket = match.captured(1).toInt();
        parsed.playFps = match.captured(2).toInt();
    } else {
        parsed.bucket = match.capturedLength(3) > 0 ? match.captured(3).toInt() : kTranscodeMaxSide;
        parsed.legacy = true;
    }
    return parsed;
}

QList<TranscodeSource> TranscodeManager::queued() const
{
    QList<TranscodeSource> sources;
    for (const Job &job : m_queue)
        sources.append(job.source);
    return sources;
}

bool TranscodeManager::cancelQueued(const TranscodeSource &source)
{
    const QString output = outputPath(source);
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->output == output) {
            m_queue.erase(it);
            emit changed(source.itemId);
            return true;
        }
    }
    return false;
}

TranscodeStatus TranscodeManager::status(const TranscodeSource &source) const
{
    TranscodeStatus status;
    status.bucket = source.bucket;
    status.playFps = source.playFps;
    const QString output = outputPath(source);
    if (m_hasCurrent && m_current.output == output) {
        status.state = TranscodeStatus::Running;
        status.progress = m_progress;
        return status;
    }
    for (const Job &job : m_queue) {
        if (job.output == output) {
            status.state = TranscodeStatus::Queued;
            return status;
        }
    }
    if (QFileInfo(output).isFile()) {
        status.state = TranscodeStatus::Done;
        status.file = output;
        return status;
    }
    for (const QString &legacy : legacyPaths(source)) {
        if (QFileInfo(legacy).isFile()) {
            status.state = TranscodeStatus::Done;
            status.file = legacy;
            return status;
        }
    }
    const auto failure = m_failures.constFind(output);
    if (failure != m_failures.cend()) {
        status.state = TranscodeStatus::Failed;
        status.error = *failure;
    }
    return status;
}

QString TranscodeManager::filterFor(double fps, int bucket, int playFps)
{
    // The longest side is capped (never upscaled) and the other side follows, kept even for the encoder.
    QString filter = u"scale='if(gt(iw,ih),min(%1,iw),-2)':'if(gt(iw,ih),-2,min(%1,ih))'"_s.arg(bucket);
    if (playFps > 0) {
        // Frame N is shown at N/playFps, whatever delay the file gives it; fps= then writes them as they are.
        return u"setpts=N/(%1*TB),"_s.arg(playFps) + filter + u",fps=%1"_s.arg(playFps);
    }
    if (fps >= kTranscodeHalveFpsFrom)
        filter += u",fps=30"_s;
    return filter;
}

QStringList TranscodeManager::ffmpegArguments(const TranscodeSource &source, const QString &outputPart)
{
    // The files come from the Workshop, so ffmpeg only ever opens this one local file (no playlists or network
    // protocols inside it) and is told the container instead of probing for it.
    QStringList input;
    const QString suffix = QFileInfo(source.file).suffix().toLower();
    const bool apng = suffix == u"apng" || suffix == u"png";
    if (apng) {
        // The pets' rule for tiny delays (petFrameDelayMs, 10 ms or less plays for 100 ms): the demuxer replaces a delay of 0, or shorter
        // than 1/max_fps, by 1/default_fps. 99 catches a delay of exactly 10 ms too (integer division of the fraction). A GIF's tiny
        // delays and an animated WebP's are not reached by demuxer options (see docs/pets/timing.md).
        input << u"-max_fps"_s << u"99"_s << u"-default_fps"_s << u"10"_s;
    }
    input << u"-protocol_whitelist"_s << u"file"_s;
    if (apng)
        input << u"-f"_s << u"apng"_s;
    else if (suffix == u"gif")
        input << u"-f"_s << u"gif"_s;
    else if (suffix == u"webp")
        input << u"-f"_s << u"webp_pipe"_s;
    input << u"-i"_s << u"file:"_s + source.file;
    return QStringList{u"-nostdin"_s, u"-hide_banner"_s, u"-v"_s, u"error"_s, u"-y"_s} + input
        + QStringList{
            u"-vf"_s, filterFor(source.fps, source.bucket, source.playFps),
            u"-c:v"_s, u"libvpx-vp9"_s, u"-pix_fmt"_s, u"yuva420p"_s,
            // Alpha is lost by VP9 when alternate reference frames are on.
            u"-auto-alt-ref"_s, u"0"_s, u"-row-mt"_s, u"1"_s, u"-threads"_s, source.bucket > kTranscodeMaxSide ? u"4"_s : u"2"_s}
        // A stage's copy is as big as the screen: the slowest setting of libvpx would take hours, so these use its good-quality real-time-ish middle and tile columns.
        + (source.bucket > kTranscodeMaxSide ? QStringList{u"-deadline"_s, u"good"_s, u"-cpu-used"_s, u"4"_s, u"-tile-columns"_s, u"2"_s, u"-lag-in-frames"_s, u"0"_s} : QStringList{})
        + QStringList{
            u"-b:v"_s, u"0"_s, u"-crf"_s, u"32"_s, u"-an"_s,
            u"-progress"_s, u"pipe:1"_s,
            u"-f"_s, u"webm"_s, outputPart};
}

bool TranscodeManager::request(const TranscodeSource &source, bool retryFailed)
{
    if (source.itemId.isEmpty() || !isTranscodeBucket(source.bucket) || source.playFps < 0 || source.playFps > kMaxPetFps || !QFileInfo(source.file).isFile())
        return false;
    const TranscodeStatus current = status(source);
    if (current.state == TranscodeStatus::Queued || current.state == TranscodeStatus::Running
        || current.state == TranscodeStatus::Done)
        return true;
    if (current.state == TranscodeStatus::Failed && !retryFailed)
        return true;
    const QString output = outputPath(source);
    m_failures.remove(output);
    // Small first: the copies a pet needs (and a small stage's) are not kept waiting behind a 4K encode that takes minutes; the same size keeps its order.
    qsizetype at = m_queue.size();
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).source.bucket > source.bucket) {
            at = i;
            break;
        }
    }
    m_queue.insert(at, {source, output});
    emit changed(source.itemId);
    if (!m_hasCurrent)
        QTimer::singleShot(0, this, &TranscodeManager::startNext);
    return true;
}

void TranscodeManager::cleanStale(const Job &job) const
{
    // Older conversions of this variant (the source changed) and leftovers of a crash. The other buckets and rates
    // of the same source state are kept: they are other conversions, not stale ones.
    const QString prefix = variantPrefix(job.source.variant);
    const QString currentKey = key(job.source);
    const QDir dir(QFileInfo(job.output).absolutePath());
    for (const QString &name : dir.entryList(QDir::Files)) {
        if (!name.startsWith(prefix))
            continue;
        const auto parsed = parseName(name.mid(prefix.size()));
        if (!parsed)
            continue;
        const QString full = dir.filePath(name);
        // One job runs at a time: any .part is a leftover.
        if (full != job.output && (parsed->key != currentKey || parsed->part))
            QFile::remove(full);
    }
    QFile::remove(job.output + u".part"_s);
}

QList<TranscodeManager::DoneCopy> TranscodeManager::doneCopies(const TranscodeSource &source) const
{
    const QString prefix = variantPrefix(source.variant);
    const QString currentKey = key(source);
    struct Found {
        QDateTime time;
        DoneCopy copy;
        bool legacy;
    };
    QList<Found> found;
    const QDir dir(dirFor(source.itemId));
    for (const QFileInfo &file : dir.entryInfoList(QDir::Files)) {
        if (!file.fileName().startsWith(prefix))
            continue;
        const auto parsed = parseName(file.fileName().mid(prefix.size()));
        if (!parsed || parsed->part || parsed->key != currentKey || !isTranscodeBucket(parsed->bucket)
            || parsed->playFps > kMaxPetFps || file.size() <= 0)
            continue;
        // A copy from before the rate existed is a copy at the file's own timing; the new name wins over it.
        const bool duplicate = std::any_of(found.cbegin(), found.cend(), [&](const Found &other) {
            return other.copy.bucket == parsed->bucket && other.copy.playFps == parsed->playFps;
        });
        if (duplicate && parsed->legacy)
            continue;
        if (!parsed->legacy) {
            found.erase(std::remove_if(found.begin(), found.end(),
                                       [&](const Found &other) {
                                           return other.copy.bucket == parsed->bucket
                                               && other.copy.playFps == parsed->playFps;
                                       }),
                        found.end());
        }
        found.append({file.lastModified(), {parsed->bucket, parsed->playFps}, parsed->legacy});
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.time != b.time)
            return a.time > b.time;
        return a.copy.bucket != b.copy.bucket ? a.copy.bucket > b.copy.bucket : a.copy.playFps > b.copy.playFps;
    });
    QList<DoneCopy> copies;
    for (const Found &one : std::as_const(found))
        copies.append(one.copy);
    return copies;
}

QList<int> TranscodeManager::doneBuckets(const TranscodeSource &source) const
{
    QList<int> buckets;
    for (const DoneCopy &copy : doneCopies(source)) {
        if (copy.playFps == source.playFps)
            buckets.append(copy.bucket);
    }
    return buckets;
}

QList<TranscodeStatus> TranscodeManager::statuses(const TranscodeSource &source) const
{
    QList<TranscodeStatus> list;
    const QString currentKey = key(source);
    const auto has = [&list](int bucket, int playFps) {
        return std::any_of(list.cbegin(), list.cend(),
                           [&](const TranscodeStatus &s) { return s.bucket == bucket && s.playFps == playFps; });
    };
    const auto same = [&](const TranscodeSource &other) {
        return other.itemId == source.itemId && other.variant == source.variant && key(other) == currentKey;
    };
    if (m_hasCurrent && same(m_current.source)) {
        TranscodeSource running = m_current.source;
        list.append(status(running));
    }
    for (const Job &job : m_queue) {
        if (same(job.source) && !has(job.source.bucket, job.source.playFps))
            list.append(status(job.source));
    }
    for (const DoneCopy &copy : doneCopies(source)) {
        if (has(copy.bucket, copy.playFps))
            continue;
        TranscodeSource one = source;
        one.bucket = copy.bucket;
        one.playFps = copy.playFps;
        list.append(status(one));
    }
    const QString prefix = dirFor(source.itemId) + u'/' + variantPrefix(source.variant);
    for (auto it = m_failures.cbegin(); it != m_failures.cend(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        const auto parsed = parseName(it.key().mid(prefix.size()));
        if (!parsed || parsed->key != currentKey || has(parsed->bucket, parsed->playFps))
            continue;
        TranscodeStatus failed;
        failed.state = TranscodeStatus::Failed;
        failed.bucket = parsed->bucket;
        failed.playFps = parsed->playFps;
        failed.error = it.value();
        list.append(failed);
    }
    return list;
}

bool TranscodeManager::removeDone(const TranscodeSource &source)
{
    const TranscodeStatus current = status(source);
    if (current.state != TranscodeStatus::Done || !QFile::remove(current.file))
        return false;
    emit changed(source.itemId);
    return true;
}

void TranscodeManager::pruneCache(const QList<TranscodeSource> &expected, const QStringList &libraryItems, int maxAgeDays)
{
    QHash<QString, QSet<QString>> keep; // item dir name -> file names to keep
    for (const TranscodeSource &source : expected) {
        keep[safeName(source.itemId)].insert(QFileInfo(outputPath(source)).fileName());
        for (const QString &legacy : legacyPaths(source))
            keep[safeName(source.itemId)].insert(QFileInfo(legacy).fileName());
    }
    for (const QString &itemId : libraryItems)
        keep[safeName(itemId)]; // known to the library, whatever is kept of it
    // What is queued or running now is not garbage either.
    for (const Job &job : std::as_const(m_queue))
        keep[safeName(job.source.itemId)].insert(QFileInfo(job.output).fileName());
    if (m_hasCurrent) {
        keep[safeName(m_current.source.itemId)].insert(QFileInfo(m_current.output).fileName());
        keep[safeName(m_current.source.itemId)].insert(QFileInfo(m_current.output).fileName() + u".part"_s);
    }

    const QDir root(m_root);
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    for (const QString &dirName : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir dir(root.filePath(dirName));
        const bool known = keep.contains(dirName);
        for (const QFileInfo &file : dir.entryInfoList(QDir::Files)) {
            const bool ours = file.fileName().endsWith(u".webm"_s) || file.fileName().endsWith(u".webm.part"_s);
            if (!ours || (known && keep[dirName].contains(file.fileName())))
                continue;
            if (!known && file.lastModified() > cutoff)
                continue; // the item may just be on a drive that is not mounted
            QFile::remove(file.filePath());
        }
        root.rmdir(dirName); // only succeeds when empty
    }
}

void TranscodeManager::startNext()
{
    if (m_hasCurrent || m_queue.isEmpty())
        return;
    m_current = m_queue.takeFirst();
    m_hasCurrent = true;
    m_progress = 0;
    m_reportedProgress = 0;
    m_outBuffer.clear();
    m_errBuffer.clear();
    m_errLines.clear();
    m_killedByWatchdog = false;
    m_killedByBudget = false;
    emit changed(m_current.source.itemId);

    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (ffmpeg.isEmpty()) {
        finish(u"ffmpeg not found"_s);
        return;
    }
    if (!QFileInfo(m_current.source.file).isFile()) {
        finish(u"source file is gone"_s);
        return;
    }
    if (!QDir().mkpath(QFileInfo(m_current.output).absolutePath())) {
        finish(u"cannot create the cache directory"_s);
        return;
    }
    cleanStale(m_current);

    m_process = std::make_unique<QProcess>();
    const QString part = m_current.output + u".part"_s;
    const QString nice = QStandardPaths::findExecutable(u"nice"_s);
    QStringList args = ffmpegArguments(m_current.source, part);
    if (nice.isEmpty()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("murasakid: nice is not available; transcoding runs at normal priority");
        }
        m_process->setProgram(ffmpeg);
    } else {
        m_process->setProgram(nice);
        args = QStringList{u"-n"_s, u"10"_s, ffmpeg} + args;
    }
    m_process->setArguments(args);
    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, &TranscodeManager::onStdout);
    connect(m_process.get(), &QProcess::readyReadStandardError, this, &TranscodeManager::onStderr);
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(u"cannot start ffmpeg: "_s + (m_process ? m_process->errorString() : QString()));
    });
    connect(m_process.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        if (!m_process)
            return;
        onStdout();
        onStderr();
        if (m_killedByBudget) {
            finish(u"the copy for a stage took too long and was stopped"_s);
        } else if (m_killedByWatchdog) {
            finish(u"ffmpeg stopped responding and was killed"_s);
        } else if (exit != QProcess::NormalExit || code != 0) {
            const QString detail = m_errLines.join(u'\n');
            finish(detail.isEmpty() ? u"ffmpeg exited with status %1"_s.arg(code) : detail);
        } else {
            finish(QString());
        }
    });
    m_watchdog.start(m_stallMs);
    if (m_current.source.bucket > kTranscodeMaxSide)
        m_budget.start(m_stageBudgetMs);
    m_process->start();
}

void TranscodeManager::onStdout()
{
    if (!m_process)
        return;
    m_watchdog.start(m_stallMs);
    m_outBuffer += m_process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_outBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_outBuffer.left(newline).trimmed();
        m_outBuffer.remove(0, newline + 1);
        if (!line.startsWith("out_time_us=") && !line.startsWith("out_time_ms="))
            continue;
        // Both keys hold microseconds (out_time_ms is a long-standing misnomer).
        bool ok = false;
        const qint64 micros = line.mid(line.indexOf('=') + 1).toLongLong(&ok);
        // The copy's own timeline: with a playback rate every frame lasts 1/rate, otherwise as the source does.
        const double rate = m_current.source.playFps > 0 ? m_current.source.playFps : m_current.source.fps;
        const double total = rate > 0 ? m_current.source.frames / rate : 0;
        if (!ok || micros < 0 || total <= 0)
            continue;
        m_progress = std::clamp(micros / 1e6 / total, 0.0, 0.99);
        if (m_progress - m_reportedProgress >= 0.02) {
            m_reportedProgress = m_progress;
            emit changed(m_current.source.itemId);
        }
    }
    if (m_outBuffer.size() > kMaxLineBytes)
        m_outBuffer.clear();
}

void TranscodeManager::onStderr()
{
    if (!m_process)
        return;
    m_watchdog.start(m_stallMs);
    m_errBuffer += m_process->readAllStandardError();
    qsizetype newline;
    while ((newline = m_errBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_errBuffer.left(newline)).trimmed();
        m_errBuffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        m_errLines.append(line.left(500));
        while (m_errLines.size() > kKeptStderrLines)
            m_errLines.removeFirst();
    }
    if (m_errBuffer.size() > kMaxLineBytes)
        m_errBuffer = m_errBuffer.right(kMaxLineBytes / 2);
}

void TranscodeManager::finish(const QString &error)
{
    m_watchdog.stop();
    m_budget.stop();
    const Job job = m_current;
    const QString part = job.output + u".part"_s;
    QString problem = error;
    if (problem.isEmpty()) {
        QFile::remove(job.output);
        if (!QFileInfo(part).isFile() || QFileInfo(part).size() == 0)
            problem = u"ffmpeg produced no output"_s;
        else if (!QFile::rename(part, job.output))
            problem = u"cannot move the result into the cache"_s;
    }
    if (!problem.isEmpty()) {
        QFile::remove(part);
        m_failures.insert(job.output, problem);
        qWarning("murasakid: transcode of %s failed: %s", qPrintable(job.source.file), qPrintable(problem.left(300)));
    }
    if (m_process) {
        // finish() can run from inside the process' own signal: destroy it afterwards.
        m_process->disconnect(this);
        m_process.release()->deleteLater();
    }
    m_hasCurrent = false;
    emit changed(job.source.itemId);
    QTimer::singleShot(0, this, &TranscodeManager::startNext);
}

} // namespace murasaki
