#include "screenrefstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QtEndian>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

bool fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return false;
}

bool validRegion(int region) { return region >= 0 && region < kMaxScreenRefRegions; }
bool validStep(int step) { return step >= 0 && step < kMaxScreenRefSteps; }

bool isPlainFolder(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink();
}

QString regionName(int region) { return u"region%1"_s.arg(region); }
QString stepName(int step) { return u"step%1.png"_s.arg(step); }

// Writes `bytes` as `<folder>/<name>` without ever following or writing through a link: a temporary file made with O_CREAT|O_EXCL|O_NOFOLLOW in the same folder (mode 0600), written
// whole and fsync'ed, then renamed over the target. rename(2) replaces the directory entry itself, so a symlink at `name` is replaced by the file (its target is not touched) and a
// hard link to another file is left alone. The folder itself is checked with lstat to be a real directory.
bool writeFileSafely(const QString &folder, const QString &name, const QByteArray &bytes, QString *error)
{
    const QByteArray dir = QFile::encodeName(folder);
    struct stat st {};
    if (::lstat(dir.constData(), &st) != 0 || !S_ISDIR(st.st_mode))
        return fail(error, u"%1 is not a real folder"_s.arg(QFileInfo(folder).fileName()));
    const int dirFd = ::open(dir.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirFd < 0)
        return fail(error, u"cannot open the folder"_s);
    QByteArray tmp;
    int fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
        tmp = ".tmp-" + QByteArray::number(quint64(QRandomGenerator::global()->generate64()), 16);
        fd = ::openat(dirFd, tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    bool ok = fd >= 0;
    if (ok) {
        qsizetype done = 0;
        while (ok && done < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.constData() + done, size_t(bytes.size() - done));
            ok = n > 0;
            done += n > 0 ? qsizetype(n) : 0;
        }
        ok = ok && ::fsync(fd) == 0;
        ::close(fd);
        ok = ok && ::renameat(dirFd, tmp.constData(), dirFd, QFile::encodeName(name).constData()) == 0;
        if (!ok)
            ::unlinkat(dirFd, tmp.constData(), 0);
        else
            ::fsync(dirFd);
    }
    ::close(dirFd);
    return ok ? true : fail(error, u"cannot write %1"_s.arg(name));
}

} // namespace

ScreenRefPngSize screenRefPngSize(const QByteArray &png)
{
    static const char kSignature[8] = {char(0x89), 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 33 || png.left(8) != QByteArrayView(kSignature, 8))
        return {};
    if (qFromBigEndian<quint32>(png.constData() + 8) != 13 || png.mid(12, 4) != "IHDR")
        return {};
    const quint32 w = qFromBigEndian<quint32>(png.constData() + 16), h = qFromBigEndian<quint32>(png.constData() + 20);
    if (w == 0 || h == 0 || w > 0x7fffffff || h > 0x7fffffff)
        return {};
    return {int(w), int(h)};
}

ScreenRefStore::ScreenRefStore(const QString &root) : m_root(root) {}

QString ScreenRefStore::defaultRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/murasaki/triggers"_s;
}

bool ScreenRefStore::validTriggerId(const QString &id)
{
    static const QRegularExpression pattern(u"\\A[A-Za-z0-9_-]{1,64}\\z"_s);
    return pattern.match(id).hasMatch();
}

QString ScreenRefStore::triggerFolder(const QString &id) const
{
    return validTriggerId(id) ? m_root + u'/' + id : QString();
}

bool ScreenRefStore::ensureFolder(const QString &path, QString *error) const
{
    if (QFileInfo::exists(path)) {
        if (!isPlainFolder(path))
            return fail(error, u"%1 is not a plain folder"_s.arg(QFileInfo(path).fileName()));
        return true;
    }
    if (!QDir().mkpath(path))
        return fail(error, u"cannot make %1"_s.arg(QFileInfo(path).fileName()));
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return true;
}

bool ScreenRefStore::putFrame(const QString &id, int region, int step, const QByteArray &png, QString *error)
{
    if (!validTriggerId(id))
        return fail(error, u"not a trigger id"_s);
    if (!validRegion(region) || !validStep(step))
        return fail(error, u"region or step out of range"_s);
    if (png.size() > kMaxScreenRefBytes)
        return fail(error, u"the picture is over %1 MiB"_s.arg(kMaxScreenRefBytes >> 20));
    const ScreenRefPngSize size = screenRefPngSize(png);
    if (size.width == 0 || qMax(size.width, size.height) > kMaxScreenRefSide)
        return fail(error, size.width == 0 ? u"not a PNG"_s : u"the picture is over %1 pixels on its longer side"_s.arg(kMaxScreenRefSide));
    if (!ensureFolder(m_root, error) || !ensureFolder(triggerFolder(id), error) || !ensureFolder(triggerFolder(id) + u'/' + regionName(region), error))
        return false;
    return writeFileSafely(triggerFolder(id) + u'/' + regionName(region), stepName(step), png, error);
}

QByteArray ScreenRefStore::frame(const QString &id, int region, int step) const
{
    if (!validTriggerId(id) || !validRegion(region) || !validStep(step))
        return {};
    const QString path = triggerFolder(id) + u'/' + regionName(region) + u'/' + stepName(step);
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() > kMaxScreenRefBytes)
        return {};
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QList<int> ScreenRefStore::steps(const QString &id, int region) const
{
    QList<int> out;
    if (!validTriggerId(id) || !validRegion(region))
        return out;
    static const QRegularExpression pattern(u"\\Astep(0|[1-9][0-9]{0,1})\\.png\\z"_s);
    for (const QString &name : QDir(triggerFolder(id) + u'/' + regionName(region)).entryList(QDir::Files | QDir::NoSymLinks)) {
        const auto m = pattern.match(name);
        if (m.hasMatch() && validStep(m.captured(1).toInt()))
            out.append(m.captured(1).toInt());
    }
    std::sort(out.begin(), out.end());
    return out;
}

QList<int> ScreenRefStore::regions(const QString &id) const
{
    QList<int> out;
    if (!validTriggerId(id))
        return out;
    static const QRegularExpression pattern(u"\\Aregion(0|[1-9][0-9]{0,1})\\z"_s);
    for (const QString &name : QDir(triggerFolder(id)).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        const auto m = pattern.match(name);
        if (m.hasMatch() && validRegion(m.captured(1).toInt()))
            out.append(m.captured(1).toInt());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool ScreenRefStore::putInfo(const QString &id, const QJsonObject &info, QString *error)
{
    if (!validTriggerId(id))
        return fail(error, u"not a trigger id"_s);
    const QByteArray bytes = QJsonDocument(info).toJson(QJsonDocument::Compact);
    if (bytes.size() > (1 << 20))
        return fail(error, u"the info is over 1 MiB"_s);
    if (!ensureFolder(m_root, error) || !ensureFolder(triggerFolder(id), error))
        return false;
    return writeFileSafely(triggerFolder(id), u"info.json"_s, bytes, error);
}

QJsonObject ScreenRefStore::info(const QString &id) const
{
    if (!validTriggerId(id))
        return {};
    const QFileInfo fi(triggerFolder(id) + u"/info.json"_s);
    if (!fi.isFile() || fi.isSymLink() || fi.size() > (1 << 20))
        return {};
    QFile file(fi.filePath());
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

int ScreenRefStore::adoptImported(const QString &id, const QString &importFolder, const QJsonObject &screenRefs, QString *error)
{
    if (!validTriggerId(id)) {
        fail(error, u"not a trigger id"_s);
        return 0;
    }
    static const QRegularExpression pattern(u"\\Ascreenrefs/region(0|[1-9][0-9]{0,1})/step(0|[1-9][0-9]{0,1})\\.png\\z"_s);
    const QString base = QDir(importFolder).canonicalPath();
    if (base.isEmpty()) {
        fail(error, u"the import folder is not there"_s);
        return 0;
    }
    // Everything is built in a staging folder of the store (its name is not a valid id), then swapped in: a refusal at any point leaves the trigger as it was.
    if (!ensureFolder(m_root, error))
        return 0;
    const QString stagingRoot = m_root + u"/.stage-"_s + QString::number(QRandomGenerator::global()->generate64(), 16);
    const auto discard = [&](const QString &why) {
        QDir(stagingRoot).removeRecursively();
        fail(error, why);
        return 0;
    };
    ScreenRefStore staging(stagingRoot);
    int kept = 0;
    QJsonArray infoRegions;
    for (const QJsonValue &rv : screenRefs.value(u"regions"_s).toArray()) {
        const QJsonObject region = rv.toObject();
        if (region.value(u"status"_s).toString() != u"ready"_s)
            continue;
        QJsonArray clip;
        for (const QJsonValue &fv : region.value(u"frames"_s).toArray()) {
            const QJsonObject f = fv.toObject();
            const QString file = f.value(u"file"_s).toString();
            const auto m = pattern.match(file);
            if (!m.hasMatch())
                return discard(u"a picture named \"%1\" is not one an import writes"_s.arg(file.left(60)));
            const int r = m.captured(1).toInt(), s = m.captured(2).toInt();
            const QFileInfo fi(base + u'/' + file);
            if (!fi.isFile() || fi.isSymLink() || !fi.canonicalFilePath().startsWith(base + u'/') || fi.size() > kMaxScreenRefBytes)
                return discard(u"the picture %1 is not a plain file of the import"_s.arg(file.left(60)));
            QFile in(fi.filePath());
            if (!in.open(QIODevice::ReadOnly))
                return discard(u"cannot read %1"_s.arg(file.left(60)));
            QString why;
            if (!staging.putFrame(id, r, s, in.readAll(), &why))
                return discard(why);
            ++kept;
            clip.append(QJsonObject{{u"step"_s, s}, {u"clipIndex"_s, f.value(u"clipIndex"_s).toInt()}});
        }
        infoRegions.append(QJsonObject{{u"region"_s, region.value(u"region"_s).toInt()}, {u"frames"_s, clip}});
    }
    if (kept == 0) {
        QDir(stagingRoot).removeRecursively();
        return 0;
    }
    QString why;
    if (!staging.putInfo(id, QJsonObject{{u"regions"_s, infoRegions}}, &why))
        return discard(why);

    const QString target = triggerFolder(id);
    QString old;
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
        old = m_root + u"/.old-"_s + QString::number(QRandomGenerator::global()->generate64(), 16);
        if (!QDir().rename(target, old))
            return discard(u"cannot replace the trigger's pictures"_s);
    }
    if (!QDir().rename(staging.triggerFolder(id), target)) {
        if (!old.isEmpty())
            QDir().rename(old, target); // put the old pictures back
        return discard(u"cannot move the new pictures in"_s);
    }
    QDir(stagingRoot).removeRecursively();
    if (!old.isEmpty()) {
        if (QFileInfo(old).isSymLink())
            QFile::remove(old);
        else
            QDir(old).removeRecursively();
    }
    return kept;
}

bool ScreenRefStore::removeRegion(const QString &id, int region)
{
    if (!validTriggerId(id) || region < 0 || region >= kMaxScreenRefRegions)
        return false;
    const QString path = triggerFolder(id) + u"/region%1"_s.arg(region);
    const QFileInfo fi(path);
    if (!fi.exists() && !fi.isSymLink())
        return true;
    if (fi.isSymLink())
        return QFile::remove(path);
    if (!fi.isDir() || !fi.canonicalFilePath().startsWith(QDir(m_root).canonicalPath() + u'/'))
        return false;
    return QDir(path).removeRecursively();
}

bool ScreenRefStore::removeTrigger(const QString &id)
{
    if (!validTriggerId(id))
        return false;
    const QString path = triggerFolder(id);
    const QFileInfo fi(path);
    if (!fi.exists() && !fi.isSymLink())
        return true;
    if (fi.isSymLink()) // never follow a link out of the store: remove the link itself
        return QFile::remove(path);
    if (!fi.isDir() || !fi.canonicalFilePath().startsWith(QDir(m_root).canonicalPath() + u'/'))
        return false;
    return QDir(path).removeRecursively();
}

QStringList ScreenRefStore::triggerIds() const
{
    QStringList out;
    for (const QString &name : QDir(m_root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
        if (validTriggerId(name))
            out.append(name);
    return out;
}

} // namespace murasaki
