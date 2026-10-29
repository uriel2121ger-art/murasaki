#include "petcheck.h"

#include "animationinfo.h"
#include "buildinfo.h"
#include "constants.h"
#include "itemrisks.h"
#include "localpets.h"
#include "pathutil.h"
#include "petlimits.h"
#include "steamlocator.h"
#include "transcodebuckets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

using namespace petcheck;

// The first `limit` UTF-16 units of `text` without cutting a surrogate pair in two.
QString clip(const QString &text, qsizetype limit)
{
    if (text.size() <= limit)
        return text;
    qsizetype size = limit;
    if (size > 0 && text.at(size - 1).isHighSurrogate())
        --size;
    return text.left(size);
}

// Control characters of a file name would reach the terminal through the stderr echo.
QString printable(QString text)
{
    for (QChar &c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || (c.unicode() >= 0x80 && c.unicode() < 0xa0))
            c = u'?';
    }
    return text;
}

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + u": "_s + detail;
}

// What a file that is not an animation says about the item, by extension. An entry is the gap its presence names.
struct Extra {
    const char *suffix;
    std::string_view degrade;
    bool container; // an item with nothing else is refused as an unsupported container
};
constexpr Extra kExtras[] = {
    {"mdaa", kMdaaNotImported, true},     {"mp4", kVideoNotImported, true},   {"m4v", kVideoNotImported, true},
    {"webm", kVideoNotImported, true},    {"mkv", kVideoNotImported, true},   {"mov", kVideoNotImported, true},
    {"avi", kVideoNotImported, true},     {"zip", kArchiveNotImported, true}, {"moc3", {}, true},
    {"mp3", kAudioNotPlayed, false},      {"wav", kAudioNotPlayed, false},    {"ogg", kAudioNotPlayed, false},
    {"flac", kAudioNotPlayed, false},     {"m4a", kAudioNotPlayed, false},    {"opus", kAudioNotPlayed, false},
};

const Extra *extraOf(const QString &suffix)
{
    const QString lower = suffix.toLower();
    for (const Extra &extra : kExtras) {
        if (lower == QLatin1String(extra.suffix))
            return &extra;
    }
    return nullptr;
}

// A file named like an animation whose content is another container (a renamed video or archive).
const Extra *extraByMagic(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return nullptr;
    const QByteArray head = file.read(12);
    if (head.startsWith("PK\x03\x04"))
        return extraOf(u"zip"_s);
    if (head.size() == 12 && head.mid(4, 4) == "ftyp")
        return extraOf(u"mp4"_s);
    if (head.startsWith("\x1a\x45\xdf\xa3"))
        return extraOf(u"mkv"_s);
    if (head.size() == 12 && head.startsWith("RIFF") && head.mid(8, 4) == "AVI ")
        return extraOf(u"avi"_s);
    return nullptr;
}

void addUnique(QStringList &list, const QString &entry)
{
    if (!list.contains(entry))
        list.append(entry);
}

// A frame cache gap of one readable animation.
void noteCacheLimits(const AnimationInfo &info, PetCheckResult &result)
{
    if (info.frames > kMaxPetFrames)
        addUnique(result.degraded, named(kTooManyFrames, QString::number(info.frames)));
    if (info.width > kMaxPetCanvasSide || info.height > kMaxPetCanvasSide)
        addUnique(result.degraded, named(kCanvasTooLarge, u"%1x%2"_s.arg(info.width).arg(info.height)));
    // The overlay decodes at the pet's display size, at most the transcode cap; the check assumes that cap as the
    // worst case. A still image is never cached.
    if (info.frames >= 2) {
        const double scale = std::min(1.0, double(kTranscodeMaxSide) / std::max(info.width, info.height));
        const double bytes = double(info.frames) * std::ceil(info.width * scale) * std::ceil(info.height * scale) * 4;
        if (bytes > double(kDefaultFrameCacheMiB) * 1024 * 1024)
            addUnique(result.degraded, named(kOverCacheBudget, u"%1 MiB"_s.arg(qint64(std::ceil(bytes / (1024 * 1024))))));
    }
}

// The video path loads each frame as one GPU texture: a canvas beyond what GPUs offer cannot play at all.
bool overVideoLimit(const AnimationInfo &info)
{
    return std::max(info.width, info.height) > kMaxPetVideoSide;
}

PetCheckResult checkFile(const QString &path)
{
    PetCheckResult result;
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
        result.reasons.append(named(kItemNotFound, clip(path, 200)));
        return result;
    }
    const QString name = clip(info.fileName(), 200);
    if (!isAnimationFile(canonical)) {
        const Extra *extra = extraOf(info.suffix());
        if (extra && extra->container)
            result.reasons.append(named(kUnsupportedContainer, info.suffix().toLower()));
        else
            result.reasons.append(named(kUnsupportedFileType, name));
        return result;
    }
    if (QFileInfo(canonical).size() > kMaxLocalPetBytes) {
        result.reasons.append(named(kTooLarge, name));
        return result;
    }
    const auto animation = readAnimationInfo(canonical);
    if (!animation) {
        const Extra *extra = extraByMagic(canonical);
        result.reasons.append(extra ? named(kUnsupportedContainer, QString::fromLatin1(extra->suffix))
                                    : named(kNotReadable, name));
        return result;
    }
    if (overVideoLimit(*animation)) {
        result.reasons.append(named(kCanvasOverVideoLimit, u"%1x%2"_s.arg(animation->width).arg(animation->height)));
        return result;
    }
    noteCacheLimits(*animation, result);
    if (!scanFileRisk(canonical).isEmpty())
        addUnique(result.degraded, named(kProgramNotRun, name));
    result.ok = true;
    return result;
}

PetCheckResult checkDirectory(const QString &dir)
{
    PetCheckResult result;
    const QString canonicalDir = QFileInfo(dir).canonicalFilePath();
    QStringList animations;
    QStringList extraDegrades;
    QStringList containers; // suffixes of the containers that would make an item with no animation unsupported
    const QFileInfoList entries = QDir(canonicalDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isAnimationFile(entry.filePath())) {
            animations.append(entry.fileName());
        } else if (const Extra *extra = extraOf(entry.suffix())) {
            if (!extra->degrade.empty()) // a live2d model is no gap: Anima Engine does not draw one either
                addUnique(extraDegrades, named(extra->degrade));
            if (extra->container)
                addUnique(containers, entry.suffix().toLower());
        }
    }
    if (animations.isEmpty()) {
        result.reasons.append(containers.isEmpty() ? named(kNoAnimation) : named(kUnsupportedContainer, containers.join(u", "_s)));
        return result;
    }

    if (animations.size() > kMaxCheckedFiles) {
        result.degraded.append(named(kTooManyFiles, QString::number(animations.size())));
        animations = animations.mid(0, kMaxCheckedFiles);
    }
    int played = 0;
    QStringList skipped;
    QString firstReason;
    for (const QString &name : std::as_const(animations)) {
        const auto setProblem = [&firstReason, &skipped, &name](const QString &reason) {
            if (firstReason.isEmpty())
                firstReason = reason;
            skipped.append(name);
        };
        // The path goes to a decoder later: a symbolic link out of the item folder is not an animation of this item.
        const QString real = containedExistingFile(canonicalDir, name);
        if (real.isEmpty()) {
            setProblem(named(kOutsideItem, clip(name, 200)));
            continue;
        }
        if (QFileInfo(real).size() > kMaxLocalPetBytes) {
            setProblem(named(kTooLarge, clip(name, 200)));
            continue;
        }
        const auto animation = readAnimationInfo(real);
        if (!animation) {
            const Extra *extra = extraByMagic(real);
            setProblem(extra ? named(kUnsupportedContainer, QString::fromLatin1(extra->suffix))
                             : named(kNotReadable, clip(name, 200)));
            continue;
        }
        if (overVideoLimit(*animation)) {
            setProblem(named(kCanvasOverVideoLimit, clip(name, 200)));
            continue;
        }
        ++played;
        noteCacheLimits(*animation, result);
    }
    if (played == 0) {
        result.reasons.append(firstReason);
        return result;
    }
    if (!skipped.isEmpty())
        result.degraded.append(named(kVariantSkipped, clip(skipped.join(u", "_s), 400)));
    result.degraded.append(extraDegrades);
    if (!scanItemRisks(canonicalDir).isEmpty())
        addUnique(result.degraded, named(kProgramNotRun));
    result.degraded.sort(); // the same item always gives the same line, whatever order the files came in
    result.ok = true;
    return result;
}

} // namespace

PetCheckResult checkPetItem(const QString &item, const QStringList &animaDirs)
{
    QString target = item;
    if (item.startsWith(u"anima:"_s)) {
        const QString id = item.mid(6);
        target.clear();
        // The id is one folder name of a Workshop content dir, never a path.
        if (!id.isEmpty() && !id.contains(u'/') && id != u"."_s && id != u".."_s) {
            QStringList roots;
            for (const QString &root : animaDirs) {
                const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
                if (!canonicalRoot.isEmpty())
                    roots.append(canonicalRoot);
            }
            for (const QString &root : std::as_const(roots)) {
                // A symbolic link <root>/<id> that leads out of the Workshop dir is not an item of it.
                const QString candidate = QFileInfo(QDir(root).filePath(id)).canonicalFilePath();
                if (!candidate.isEmpty() && QFileInfo(candidate).isDir() && candidate != root
                    && isInsideAnyRoot(candidate, roots)) {
                    target = candidate;
                    break;
                }
            }
        }
        if (target.isEmpty()) {
            PetCheckResult result;
            result.reasons.append(named(kItemNotFound, clip(item, 200)));
            return result;
        }
    }
    const QFileInfo info(target);
    if (info.isDir())
        return checkDirectory(target);
    return checkFile(target);
}

QStringList defaultAnimaDirs()
{
    const SteamLocator steam(SteamLocator::defaultRootCandidates());
    QStringList dirs = steam.workshopDirCandidates(kAnimaEngineAppId);
    dirs.append(SteamLocator::steamCmdContentDir(SteamLocator::steamCmdInstallDir(), kAnimaEngineAppId));
    dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [](const QString &dir) { return !QFileInfo(dir).isDir(); }), dirs.end());
    return dirs;
}

QByteArray petCheckJson(const PetCheckResult &result)
{
    const auto array = [](const QStringList &list) {
        return QJsonDocument(QJsonArray::fromStringList(list)).toJson(QJsonDocument::Compact);
    };
    return QByteArray("{\"ok\":") + (result.ok ? "true" : "false") + ",\"reasons\":" + array(result.reasons)
        + ",\"degraded\":" + array(result.degraded) + ",\"engine\":\"murasaki-overlay\",\"version\":"
        + QJsonDocument(QJsonArray{versionString()}).toJson(QJsonDocument::Compact).mid(1).chopped(1) + "}";
}

int runPetCheck(const QString &item)
{
    const PetCheckResult result = checkPetItem(item, defaultAnimaDirs());
    std::puts(petCheckJson(result).constData());
    if (!result.degraded.isEmpty())
        std::fprintf(stderr, "degraded: %s\n", qPrintable(printable(result.degraded.join(u"; "_s))));
    return 0;
}

} // namespace murasaki
