#include "mdaaimport.h"

#include "animationinfo.h"
#include "boundedzip.h"
#include "imagesniff.h"
#include "importspec.h"
#include "mddoparser.h"
#include "petimport.h"
#include "screenframes.h"
#include "sourcefile.h"
#include "triggerspack.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QSet>

#include <algorithm>

namespace murasaki::import {

namespace {

constexpr int kMaxVariants = 256;
constexpr long long kMaxMemberBytes = 512LL << 20;
constexpr qsizetype kMaxSmallMember = 16 << 20; // miniatura.png and actions.mddo are read into memory
constexpr long long kMaxNpzBytes = 300LL << 20; // one frames file, put in a temporary file of the job folder while it is read

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

bool isProgramName(const QString &name)
{
    static const QStringList extensions{QStringLiteral(".exe"), QStringLiteral(".dll"), QStringLiteral(".so"), QStringLiteral(".sh"), QStringLiteral(".bat"),
                                        QStringLiteral(".cmd"), QStringLiteral(".msi"), QStringLiteral(".com"), QStringLiteral(".scr"), QStringLiteral(".py"),
                                        QStringLiteral(".pyd"), QStringLiteral(".jar"), QStringLiteral(".appimage")};
    return std::any_of(extensions.begin(), extensions.end(), [&](const QString &e) { return name.endsWith(e, Qt::CaseInsensitive); });
}

bool isAudioName(const QString &name)
{
    static const QStringList extensions{QStringLiteral(".mp3"), QStringLiteral(".wav"), QStringLiteral(".ogg"), QStringLiteral(".flac"), QStringLiteral(".m4a")};
    return std::any_of(extensions.begin(), extensions.end(), [&](const QString &e) { return name.endsWith(e, Qt::CaseInsensitive); });
}

QString leafOf(const QString &member)
{
    return member.mid(member.lastIndexOf(QLatin1Char('/')) + 1);
}

// The relative member name an actions.mddo path means, or an empty string when it leaves the package.
QString resolveInPackage(const QString &written)
{
    QString path = written;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QStringLiteral("./")))
        path.remove(0, 2);
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || (path.size() >= 2 && path.at(1) == QLatin1Char(':')) || path.contains(QChar(0)))
        return {};
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString &p : parts)
        if (p.isEmpty() || p == QStringLiteral(".") || p == QStringLiteral(".."))
            return {};
    return path.toLower();
}

const char *mediaExtension(const char *format, const QByteArray &)
{
    if (qstrcmp(format, "gif") == 0)
        return "gif";
    if (qstrcmp(format, "webp") == 0)
        return "webp";
    return "apng"; // a PNG that is an animation
}

// A file that refuses to grow past what is left of the budget: the archive's bytes are counted as they are written, so a package that inflates to more than
// the limit stops there and not after it has filled the disk.
class BudgetFile : public QFile
{
public:
    BudgetFile(const QString &name, long long *left) : QFile(name), m_left(left) {}

protected:
    qint64 writeData(const char *data, qint64 length) override
    {
        if (length > *m_left)
            return -1;
        const qint64 n = QFile::writeData(data, length);
        if (n > 0)
            *m_left -= n;
        return n;
    }

private:
    long long *m_left;
};

// The reference pictures of the pack's screen regions: for each region of pack.json the matching frames/region<i>.npz (or region_<i>.npz) is put in a temporary file of the job
// folder, read with the strict .npy reader, and only the frames the trigger uses are kept (screenframes.h). Nothing is ever loaded from it but numbers.
QJsonObject importScreenRefs(const BoundedZip &zip, const QHash<QString, int> &byName, const QJsonObject &pack, const QString &folder, long long *left,
                             const std::function<bool()> &stop, QElapsedTimer *clock, const MdaaBudget &budget, MdaaResult *result)
{
    const int regionCount = int(pack.value(QStringLiteral("regions")).toArray().size());
    if (regionCount == 0)
        return {};
    // Only the names the facts give map to a region: frames/region<i>.npz (an exported package) or frames/region_<i>.npz (a saved folder), i without leading zeros. If both exist for
    // one region neither is used; any other .npz name is named and ignored. So the choice never depends on the order the archive lists its members.
    static const QRegularExpression pattern(QStringLiteral("\\Aframes/region(_?)(0|[1-9][0-9]{0,2})\\.npz\\z"));
    QHash<int, int> memberOfRegion;
    QSet<int> ambiguous;
    QStringList names = byName.keys();
    std::sort(names.begin(), names.end());
    for (const QString &name : names) {
        if (!name.endsWith(QStringLiteral(".npz"), Qt::CaseInsensitive))
            continue;
        const auto m = pattern.match(name);
        if (!m.hasMatch()) {
            if (name.startsWith(QStringLiteral("frames/")))
                result->notes.append(named(kScreenRefsUnknownRegion, QStringLiteral("%1 is not a name this reader maps to a region").arg(leafOf(name).left(60))));
            continue;
        }
        const int region = m.captured(2).toInt();
        if (region >= regionCount) {
            result->notes.append(named(kScreenRefsUnknownRegion, QStringLiteral("region %1").arg(region)));
            continue;
        }
        if (memberOfRegion.contains(region))
            ambiguous.insert(region);
        else
            memberOfRegion.insert(region, byName.value(name));
    }
    const ScreenRefPlan plan = screenRefPlanFromPack(pack);
    QJsonArray regions;
    bool allReady = true;
    QString firstReason;
    int framesKept = 0;
    for (int r = 0; r < regionCount; ++r) {
        ScreenRefRegion done;
        done.region = r;
        if (r >= kMaxRefRegions) {
            if (r == kMaxRefRegions)
                result->notes.append(named(kScreenRefsOverLimit, QStringLiteral("regions after the %1st").arg(kMaxRefRegions)));
            break;
        }
        if (stop && stop()) {
            result->error = named(kCancelled);
            break;
        }
        if (budget.maxMillis > 0 && clock->elapsed() > budget.maxMillis) {
            result->error = named(kTimeBudget);
            break;
        }
        const int index = memberOfRegion.value(r, -1);
        if (ambiguous.contains(r)) {
            done.status = named(kScreenRefsAmbiguous, QStringLiteral("region %1").arg(r));
        } else if (index < 0) {
            done.status = named(kScreenRefsNoFrames, QStringLiteral("region %1").arg(r));
        } else if (zip.entries().at(index).tooBig || zip.entries().at(index).size > kMaxNpzBytes) {
            done.status = named(kNpyOverLimit, QStringLiteral("the frames file of region %1").arg(r));
        } else if (framesKept >= kMaxRefFramesTotal) {
            done.status = named(kScreenRefsOverLimit, QStringLiteral("region %1").arg(r));
        } else {
            QTemporaryFile temp(QDir(folder).filePath(QStringLiteral("npz-XXXXXX.tmp")));
            QString why;
            // Cancellation and the time budget are asked while the frames file is copied out and while it is read, not only between regions.
            const auto alive = [&]() { return !(stop && stop()) && !(budget.maxMillis > 0 && clock->elapsed() > budget.maxMillis); };
            const bool copied = temp.open()
                && zip.streamTo(index, [&](const char *data, qsizetype length) { return alive() && temp.write(data, length) == length; }, &why) && temp.flush() && temp.seek(0);
            if (!copied) {
                done.status = !alive() ? named(kCancelled) : (why.isEmpty() ? named(kHelperFailed, QStringLiteral("the frames file of region %1").arg(r)) : why);
            } else {
                done = extractRegionRefs(&temp, r, plan, folder, left, kMaxRefFramesTotal - framesKept, alive);
                framesKept += int(done.frames.size());
            }
            if (!alive()) { // stopped or out of time inside this region: the whole import ends, as between regions
                result->error = (stop && stop()) ? named(kCancelled) : named(kTimeBudget);
                break;
            }
        }
        if (!done.ready()) {
            allReady = false;
            if (firstReason.isEmpty())
                firstReason = done.status;
            result->notes.append(done.status);
        }
        result->notes.append(done.notes);
        QJsonArray frames;
        for (const ScreenRefFrame &f : done.frames)
            frames.append(QJsonObject{{QStringLiteral("file"), f.file}, {QStringLiteral("clipIndex"), f.clipIndex}});
        regions.append(QJsonObject{{QStringLiteral("region"), r}, {QStringLiteral("status"), done.status}, {QStringLiteral("sourceFrames"), done.sourceFrames}, {QStringLiteral("frames"), frames}});
    }
    if (regionCount > kMaxRefRegions)
        allReady = false;
    return QJsonObject{{QStringLiteral("status"), allReady ? QStringLiteral("ready") : (firstReason.isEmpty() ? named(kScreenRefsOverLimit) : firstReason)},
                       {QStringLiteral("sourceRegions"), regionCount},
                       {QStringLiteral("regions"), regions}};
}

} // namespace

MdaaResult importMdaa(const QString &zipPath, const QString &folder, const std::function<bool()> &cancelled, const std::function<void(double)> &progress,
                      const MdaaBudget &budget)
{
    QElapsedTimer clock;
    clock.start();
    long long left = budget.maxBytes;
    MdaaResult result;
    QFile file(zipPath);
    if (!openSource(file)) {
        result.error = named(kSourceNotFound, QFileInfo(zipPath).fileName().left(80));
        return result;
    }
    ZipLimits limits;
    limits.maxMemberBytes = kMaxMemberBytes;
    limits.maxMemoryBytes = kMaxSmallMember;
    limits.skipOversize = true;
    QString why;
    auto zip = BoundedZip::open(&file, limits, &why);
    if (!zip) {
        result.error = why.startsWith(named(kZipNotAZip)) ? named(kMdaaNotAZip, why.mid(named(kZipNotAZip).size()).remove(0, 2)) : why;
        return result;
    }

    const auto stop = [&] { return cancelled && cancelled(); };
    const QList<ZipEntry> &entries = zip->entries();
    QHash<QString, int> byName; // lower-case normalised name -> index
    for (int i = 0; i < entries.size(); ++i)
        if (!entries.at(i).isDirectory)
            byName.insert(entries.at(i).name.toLower(), i);

    int thumbnail = byName.value(QStringLiteral("miniatura.png"), -1);
    const int table = byName.value(QStringLiteral("actions.mddo"), -1);
    result.triggersPack = byName.contains(QStringLiteral("pack.json"));

    // Programs first: core.exe is hashed (streamed, dropped), the others are only named.
    QSet<int> handled;
    for (int i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries.at(i);
        if (e.isDirectory || !isProgramName(e.name))
            continue;
        handled.insert(i);
        if (e.name.compare(QStringLiteral("core.exe"), Qt::CaseInsensitive) == 0) {
            const auto hash = zip->sha256(i, &why);
            result.executableName = e.name;
            result.executableSha256 = hash ? QString::fromLatin1(hash->toHex()) : QString();
            if (!result.degraded.contains(named(kMdaaExecutableNotRun)))
                result.degraded.append(named(kMdaaExecutableNotRun));
        } else {
            result.programs.append(e.name.left(120));
            result.degraded.append(named(kProgramNotRun) + QStringLiteral(": ") + e.name.left(80));
        }
    }

    // Media: a member whose first bytes say gif, webp or png, written under a name of ours and read back by the header readers.
    QSet<QString> used;
    QHash<QString, QString> variantOfMember; // member -> variant name
    int examined = 0;
    for (int i = 0; i < entries.size(); ++i) {
        if (stop()) {
            result.error = named(kCancelled);
            return result;
        }
        if (budget.maxMillis > 0 && clock.elapsed() > budget.maxMillis) {
            result.error = named(kTimeBudget);
            return result;
        }
        if (progress)
            progress(double(++examined) / double(entries.size()) * 0.9);
        const ZipEntry &e = entries.at(i);
        if (e.isDirectory || handled.contains(i) || i == table || i == thumbnail)
            continue;
        if (e.tooBig) {
            result.notes.append(named(kMdaaMediaRefused, QStringLiteral("%1: over the member limit").arg(leafOf(e.name).left(80))));
            continue;
        }
        if (isAudioName(e.name) && !result.degraded.contains(named(kAudioNotPlayed)))
            result.degraded.append(named(kAudioNotPlayed));
        const auto head = zip->head(i, 32, &why);
        const char *format = head ? sniffImageFormat(*head) : nullptr;
        if (!format || (qstrcmp(format, "jpeg") == 0)) {
            ++result.ignored;
            continue;
        }
        if (result.variants.size() >= kMaxVariants) {
            result.notes.append(named(kMdaaMediaRefused, QStringLiteral("%1: more than %2 animations").arg(leafOf(e.name).left(80)).arg(kMaxVariants)));
            continue;
        }
        QString base = leafOf(e.name);
        const int dot = base.lastIndexOf(QLatin1Char('.'));
        const QString stem = sanitiseOutputName(dot > 0 ? base.left(dot) : base);
        QString fileName = stem + QLatin1Char('.') + QLatin1String(mediaExtension(format, *head));
        for (int n = 2; used.contains(fileName.toLower()); ++n)
            fileName = stem + QStringLiteral("-%1.").arg(n) + QLatin1String(mediaExtension(format, *head));
        used.insert(fileName.toLower());
        const QString target = QDir(folder).filePath(fileName);
        const long long before = left;
        BudgetFile out(target, &left);
        if (!out.open(QIODevice::WriteOnly) || !zip->extractTo(i, &out, &why)) {
            out.close();
            QFile::remove(target);
            used.remove(fileName.toLower());
            const bool overBudget = before < (long long)e.size; // judged on what was left before this member
            left = before;                                    // a member that failed leaves nothing behind: its bytes are given back
            if (overBudget) { // not a damaged member: the package would not fit
                result.error = named(kOutputOverBudget, QStringLiteral("%1 would pass the output limit").arg(leafOf(e.name).left(80)));
                return result;
            }
            result.notes.append(named(kMdaaMediaRefused, QStringLiteral("%1: %2").arg(leafOf(e.name).left(80), why.left(120))));
            continue;
        }
        out.close();
        const auto info = readAnimationInfo(target);
        if (!info || info->frames < 2) {
            QFile::remove(target);
            used.remove(fileName.toLower());
            if (info) // a still image: not an animation, not an error
                ++result.ignored;
            else
                result.notes.append(named(kMdaaMediaRefused, leafOf(e.name).left(80)));
            continue;
        }
        MdaaVariant v;
        v.name = fileName.left(fileName.lastIndexOf(QLatin1Char('.')));
        v.file = fileName;
        v.member = e.name.toLower();
        v.frames = info->frames;
        v.width = info->width;
        v.height = info->height;
        variantOfMember.insert(v.member, v.name);
        result.variants.append(v);
    }
    if (result.variants.isEmpty()) {
        result.error = named(kMdaaNoMedia);
        return result;
    }

    // The thumbnail: a PNG of sane size, kept as the preview.
    if (thumbnail >= 0) {
        const auto bytes = zip->read(thumbnail, &why);
        const QSize size = bytes ? pngHeaderSize(bytes->left(24)) : QSize();
        if (bytes && size.isValid() && size.width() >= 1 && size.height() >= 1 && size.width() <= 4096 && size.height() <= 4096) {
            BudgetFile out(QDir(folder).filePath(QStringLiteral("preview.png")), &left);
            if (out.open(QIODevice::WriteOnly) && out.write(*bytes) == bytes->size())
                result.preview = QStringLiteral("preview.png");
        } else {
            result.notes.append(named(kMdaaMediaRefused, QStringLiteral("miniatura.png")));
        }
    }

    // The table: lines whose file is a member of the package become scenes; the rest are named and dropped.
    if (table >= 0) {
        const auto bytes = zip->read(table, &why);
        if (!bytes) {
            result.notes.append(named(kMdaaTableRefused, why.left(120)));
        } else {
            const MddoTable parsed = parseMddo(*bytes);
            if (!parsed.ok()) {
                result.notes.append(parsed.error);
            } else {
                for (const MddoRefusal &r : parsed.refused)
                    result.notes.append(QStringLiteral("mdaa: actions.mddo line %1 refused: %2").arg(r.line).arg(r.reason));
                for (const MddoAction &a : parsed.actions) {
                    const QString member = resolveInPackage(a.path);
                    if (member.isEmpty() || !byName.contains(member)) {
                        result.notes.append(named(kMdaaPathOutside, QStringLiteral("line %1").arg(a.line)));
                        continue;
                    }
                    MdaaScene scene;
                    scene.line = a.line;
                    scene.show = a.show;
                    scene.hasPosition = a.hasPosition;
                    scene.x = a.x;
                    scene.y = a.y;
                    scene.variant = variantOfMember.value(member);
                    if (a.show && scene.variant.isEmpty()) {
                        result.notes.append(named(kMdaaMediaRefused, QStringLiteral("line %1: the file is not an animation").arg(a.line)));
                        continue;
                    }
                    result.scenes.append(scene);
                }
            }
        }
    }
    if (result.triggersPack) {
        result.degraded.append(named(kMdaaTriggersPack));
        // pack.json and meta.json are data, read with the same bounds as the table; the captured frames (.npz) are only counted, never opened.
        QByteArray packJson, metaJson;
        bool said = false;
        const int packIndex = byName.value(QStringLiteral("pack.json"), -1);
        if (packIndex >= 0 && !entries.at(packIndex).tooBig) {
            const auto bytes = zip->read(packIndex, &why);
            if (bytes)
                packJson = *bytes;
            else
                result.notes.append(named(kMdaaPackRefused, why.left(120)));
            said = !bytes;
        } else {
            result.notes.append(named(kMdaaPackRefused, QStringLiteral("pack.json is not at the root of the package or is over the member limit")));
            said = true;
        }
        if (const int metaIndex = byName.value(QStringLiteral("meta.json"), -1); metaIndex >= 0 && !entries.at(metaIndex).tooBig) {
            if (const auto bytes = zip->read(metaIndex, &why))
                metaJson = *bytes;
        }
        int frames = 0;
        for (const ZipEntry &e : entries)
            frames += !e.isDirectory && e.name.endsWith(QStringLiteral(".npz"), Qt::CaseInsensitive);
        const bool hadFrames = frames > 0;
        if (!packJson.isEmpty()) {
            QHash<QString, QString> byBasename; // lower-case base name of an imported member -> its variant
            for (auto it = variantOfMember.cbegin(); it != variantOfMember.cend(); ++it)
                byBasename.insert(leafOf(it.key()), it.value());
            const TriggersPack parsed = parseTriggersPack(packJson, metaJson, byBasename);
            if (parsed.ok())
                result.pack = parsed.json;
            else
                result.notes.append(parsed.error);
            result.notes.append(parsed.notes);
            if (parsed.ok()) {
                result.screenRefs = importScreenRefs(*zip, byName, parsed.json, folder, &left, cancelled, &clock, budget, &result);
                if (!result.error.isEmpty())
                    return result;
                if (result.screenRefs.value(QStringLiteral("status")).toString() != QLatin1String("ready") && !result.screenRefs.isEmpty() && !result.degraded.contains(named(kMdaaFramesNotRead)))
                    result.degraded.append(named(kMdaaFramesNotRead));
            }
        } else if (!said) {
            result.notes.append(named(kMdaaPackRefused, QStringLiteral("pack.json is empty")));
        }
        if (hadFrames && result.pack.isEmpty() && !result.degraded.contains(named(kMdaaFramesNotRead)))
            result.degraded.append(named(kMdaaFramesNotRead)); // the pack was refused: its frames could not be placed
    }
    if (progress)
        progress(1.0);
    return result;
}

MdaaPreview previewMdaa(const QString &zipPath)
{
    MdaaPreview preview;
    QFile file(zipPath);
    if (!openSource(file)) {
        preview.error = named(kSourceNotFound, QFileInfo(zipPath).fileName().left(80));
        return preview;
    }
    ZipLimits limits;
    limits.maxMemberBytes = kMaxMemberBytes;
    limits.skipOversize = true;
    QString why;
    auto zip = BoundedZip::open(&file, limits, &why);
    if (!zip) {
        preview.error = why.startsWith(named(kZipNotAZip)) ? named(kMdaaNotAZip, why.mid(named(kZipNotAZip).size()).remove(0, 2)) : why;
        return preview;
    }
    for (int i = 0; i < zip->entries().size(); ++i) {
        const ZipEntry &e = zip->entries().at(i);
        if (e.isDirectory || e.tooBig)
            continue;
        if (e.name.compare(QStringLiteral("pack.json"), Qt::CaseInsensitive) == 0)
            preview.triggersPack = true;
        if (isProgramName(e.name)) {
            if (e.name.compare(QStringLiteral("core.exe"), Qt::CaseInsensitive) == 0)
                preview.hasExecutable = true;
            continue;
        }
        if (e.name.compare(QStringLiteral("miniatura.png"), Qt::CaseInsensitive) == 0 || e.name.compare(QStringLiteral("actions.mddo"), Qt::CaseInsensitive) == 0)
            continue;
        const auto head = zip->head(i, 32, &why);
        const char *format = head ? sniffImageFormat(*head) : nullptr;
        if (format && qstrcmp(format, "jpeg") != 0)
            ++preview.animations;
    }
    if (preview.animations == 0)
        preview.error = named(kMdaaNoMedia);
    if (preview.hasExecutable)
        preview.degraded.append(named(kMdaaExecutableNotRun));
    if (preview.triggersPack)
        preview.degraded.append(named(kMdaaTriggersPack));
    return preview;
}

QJsonObject mdaaRecord(const MdaaResult &r)
{
    QJsonArray variants, scenes;
    for (const MdaaVariant &v : r.variants)
        variants.append(QJsonObject{{QStringLiteral("name"), v.name}, {QStringLiteral("file"), v.file}, {QStringLiteral("frames"), v.frames},
                                    {QStringLiteral("width"), v.width}, {QStringLiteral("height"), v.height}});
    for (const MdaaScene &s : r.scenes) {
        QJsonObject o{{QStringLiteral("line"), s.line}, {QStringLiteral("show"), s.show}};
        if (!s.variant.isEmpty())
            o.insert(QStringLiteral("variant"), s.variant);
        if (s.hasPosition) {
            o.insert(QStringLiteral("x"), s.x);
            o.insert(QStringLiteral("y"), s.y);
        }
        scenes.append(o);
    }
    QJsonObject o{{QStringLiteral("variants"), variants},
                  {QStringLiteral("scenes"), scenes},
                  {QStringLiteral("notes"), QJsonArray::fromStringList(r.notes)},
                  {QStringLiteral("programs"), QJsonArray::fromStringList(r.programs)},
                  {QStringLiteral("thumbnail"), r.preview},
                  {QStringLiteral("triggersPack"), r.triggersPack},
                  {QStringLiteral("pack"), r.pack},
                  {QStringLiteral("screenRefs"), r.screenRefs},
                  {QStringLiteral("ignored"), r.ignored}};
    if (!r.executableName.isEmpty())
        o.insert(QStringLiteral("executable"), QJsonObject{{QStringLiteral("name"), r.executableName}, {QStringLiteral("sha256"), r.executableSha256}});
    return o;
}

} // namespace murasaki::import
