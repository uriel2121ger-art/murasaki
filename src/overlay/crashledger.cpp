#include "crashledger.h"

#include "jsonfile.h"
#include "puppetlimits.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace murasaki {

CrashLedger::CrashLedger(QString dir)
    : m_dir(std::move(dir))
{
}

QString CrashLedger::markerPath(const QString &petId) const
{
    const QByteArray hash = QCryptographicHash::hash(petId.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return m_dir + u"/opening-"_s + QString::fromLatin1(hash) + u".json"_s;
}

CrashLedger::Record CrashLedger::signatureOf(const QString &file)
{
    const QFileInfo info(file);
    Record record;
    record.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    record.size = info.exists() ? info.size() : -1;
    return record;
}

void CrashLedger::load()
{
    m_records.clear();
    const JsonFileRead read = readJsonObjectFile(m_dir + u"/crashes.json"_s);
    if (read.exists && read.problem.isEmpty() && read.root.value(u"version"_s).toInt() == 1) {
        for (const QJsonValue &value : read.root.value(u"files"_s).toArray()) {
            const QJsonObject object = value.toObject();
            const QString file = object.value(u"file"_s).toString();
            if (file.isEmpty() || m_records.size() >= kMaxFiles)
                continue;
            Record record;
            record.mtimeMs = qint64(object.value(u"mtime"_s).toDouble());
            record.size = qint64(object.value(u"size"_s).toDouble(-1));
            record.crashes = std::clamp(object.value(u"crashes"_s).toInt(), 0, 1000);
            record.at = qint64(object.value(u"at"_s).toDouble());
            m_records.insert(file, record);
        }
    } else if (read.exists && !read.problem.isEmpty()) {
        qWarning("murasaki-overlay: ignoring the crash record: %s", qPrintable(read.problem));
    }

    // A marker nobody cleared: the process died while that file was opening. The controller opens one file at a
    // time, so more than one marker means the culprit cannot be told apart: nobody is charged.
    bool changed = false;
    const QDir dir(m_dir);
    const QStringList names = dir.entryList({u"opening-*.json"_s}, QDir::Files);
    const bool ambiguous = names.size() > 1;
    if (ambiguous)
        qWarning("murasaki-overlay: the last run ended with %d files opening; not blaming any of them",
                 int(names.size()));
    for (const QString &name : names) {
        const QString path = dir.filePath(name);
        const JsonFileRead marker = readJsonObjectFile(path);
        QFile::remove(path);
        const QString file = marker.root.value(u"file"_s).toString();
        if (ambiguous || !marker.exists || !marker.problem.isEmpty() || file.isEmpty())
            continue;
        Record opened;
        opened.mtimeMs = qint64(marker.root.value(u"mtime"_s).toDouble());
        opened.size = qint64(marker.root.value(u"size"_s).toDouble(-1));
        Record &record = m_records[file];
        if (record.crashes == 0 || record.mtimeMs != opened.mtimeMs || record.size != opened.size) {
            record = opened;
            record.crashes = 0;
        }
        ++record.crashes;
        record.at = QDateTime::currentSecsSinceEpoch();
        changed = true;
        qWarning("murasaki-overlay: the last run ended while opening %s (crash %d of %d)", qPrintable(file),
                 record.crashes, kMaxCrashes);
    }
    if (changed)
        save();
}

int CrashLedger::crashes(const QString &file) const
{
    const auto it = m_records.constFind(file);
    if (it == m_records.cend())
        return 0;
    const Record now = signatureOf(file);
    return it->mtimeMs == now.mtimeMs && it->size == now.size ? it->crashes : 0;
}

void CrashLedger::markOpening(const QString &petId, const QString &file)
{
    const Record signature = signatureOf(file);
    // Atomic, so a crash right after cannot leave half a marker. A failure only means this opening is unguarded.
    const bool written = writeJsonObjectFile(markerPath(petId), QJsonObject{{u"file"_s, file},
                                                                           {u"mtime"_s, double(signature.mtimeMs)},
                                                                           {u"size"_s, double(signature.size)}});
    if (!written && !m_warnedWrite) {
        m_warnedWrite = true;
        qWarning("murasaki-overlay: cannot write the opening marker in %s; a crash will not be recorded", qPrintable(m_dir));
    }
}

void CrashLedger::clearOpening(const QString &petId)
{
    QFile::remove(markerPath(petId));
}

static_assert(CrashLedger::kCrashForgetSeconds == murasaki::puppet::kCrashForgetHours * 3600, "the ledger and the limits table agree");

void CrashLedger::recordCrash(const QString &file, bool afterLongRun, qint64 nowSecs)
{
    if (nowSecs < 0)
        nowSecs = QDateTime::currentSecsSinceEpoch();
    const Record now = signatureOf(file);
    Record &record = m_records[file];
    const bool changed = record.crashes == 0 || record.mtimeMs != now.mtimeMs || record.size != now.size;
    const bool forgotten = afterLongRun || nowSecs - record.at > kCrashForgetSeconds;
    if (changed || forgotten) {
        record = now;
        record.crashes = 0;
    }
    ++record.crashes;
    record.at = nowSecs;
    qWarning("murasaki-overlay: %s took its engine down (crash %d of %d%s)", qPrintable(file), record.crashes, kMaxCrashes, afterLongRun ? ", after a long run" : (changed ? ", first" : ""));
    save();
}

void CrashLedger::forget(const QString &file)
{
    if (m_records.remove(file))
        save();
}

void CrashLedger::save()
{
    std::vector<std::pair<QString, Record>> sorted;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
        sorted.emplace_back(it.key(), it.value());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.at > b.second.at; });
    if (sorted.size() > size_t(kMaxFiles)) {
        sorted.resize(kMaxFiles); // the least recent records go
        m_records.clear();
        for (const auto &[file, record] : sorted)
            m_records.insert(file, record);
    }
    QJsonArray files;
    for (const auto &[file, record] : sorted) {
        files.append(QJsonObject{{u"file"_s, file},
                                 {u"mtime"_s, double(record.mtimeMs)},
                                 {u"size"_s, double(record.size)},
                                 {u"crashes"_s, record.crashes},
                                 {u"at"_s, double(record.at)}});
    }
    writeJsonObjectFile(m_dir + u"/crashes.json"_s, QJsonObject{{u"version"_s, 1}, {u"files"_s, files}});
}

} // namespace murasaki
