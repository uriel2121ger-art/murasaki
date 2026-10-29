#include "localpets.h"

#include "animationinfo.h"
#include "itemrisks.h"
#include "jsonfile.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// The record of a package folder an import made (src/import/importjob.cpp writes it): its "mdaa" object lists the animation files of the folder. Only a
// plain file name counts; nothing outside the folder is ever named.
QJsonObject packageRecord(const QString &dir)
{
    QFile file(dir + u"/source.json"_s);
    if (!file.open(QIODevice::ReadOnly) || file.size() > (1 << 20))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    return root.value(u"mdaa"_s).toObject().isEmpty() ? QJsonObject() : root;
}

QStringList packageFiles(const QString &dir, const QJsonObject &record)
{
    QStringList files;
    for (const QJsonValue &v : record.value(u"mdaa"_s).toObject().value(u"variants"_s).toArray()) {
        const QString name = v.toObject().value(u"file"_s).toString();
        if (name.isEmpty() || name.contains(u'/') || name.startsWith(u'.') || files.size() >= 256)
            continue;
        if (QFileInfo(dir + u'/' + name).isFile())
            files.append(name);
    }
    return files;
}

} // namespace

QString LocalPetImporter::validate(const QString &path, QString *error)
{
    if (path.isEmpty() || path.size() > 4096 || !QFileInfo(path).isAbsolute()) {
        fail(error, u"path must be absolute"_s);
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty() && QFileInfo(canonical).isDir()) { // a folder an import made from a package: one pet, one variant per animation
        const QJsonObject record = packageRecord(canonical);
        if (record.isEmpty() || packageFiles(canonical, record).isEmpty()) {
            fail(error, u"not an imported package folder: "_s + info.fileName().left(200));
            return {};
        }
        return canonical;
    }
    if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
        fail(error, u"not a file: "_s + path.left(200));
        return {};
    }
    if (QFileInfo(canonical).size() > kMaxLocalPetBytes) {
        fail(error, u"file too large (at most 2 GiB): "_s + info.fileName().left(200));
        return {};
    }
    if (!isAnimationFile(canonical)) {
        fail(error, u"unsupported file type (gif, webp, apng and a png that is animated are supported): "_s + info.fileName().left(200));
        return {};
    }
    if (!readAnimationInfo(canonical)) {
        fail(error, u"not a readable animation: "_s + info.fileName().left(200));
        return {};
    }
    return canonical;
}

QString LocalPetImporter::itemIdFor(const QString &canonicalPath)
{
    return u"local:"_s
        + QString::fromLatin1(QCryptographicHash::hash(canonicalPath.toUtf8(), QCryptographicHash::Sha1).toHex());
}

WallpaperItem LocalPetImporter::scanItem(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir()) {
        WallpaperItem item;
        item.id = itemIdFor(path);
        item.source = u"local"_s;
        item.dir = path;
        item.contentDir = path;
        item.kind = ItemKind::Pet;
        const QJsonObject record = packageRecord(path);
        const QString name = record.value(u"spec"_s).toObject().value(u"name"_s).toString().left(200);
        item.title = name.isEmpty() ? info.fileName() : name;
        if (QFileInfo(path + u"/preview.png"_s).isFile())
            item.previewPath = path + u"/preview.png"_s;
        for (const QString &fileName : packageFiles(path, record)) {
            const QString file = path + u'/' + fileName;
            const auto animation = readAnimationInfo(file);
            if (!animation)
                continue;
            PetVariant variant;
            variant.name = QFileInfo(file).completeBaseName();
            variant.file = file;
            variant.width = animation->width;
            variant.height = animation->height;
            variant.fps = animation->fps;
            variant.frames = animation->frames;
            variant.bytes = QFileInfo(file).size();
            item.variants.append(std::move(variant));
        }
        std::sort(item.variants.begin(), item.variants.end(), [](const PetVariant &a, const PetVariant &b) { return a.name < b.name; });
        if (item.variants.isEmpty()) {
            item.status = ItemStatus::Invalid;
            item.statusDetail = info.exists() ? info.fileName() + u": no readable animation"_s : info.fileName() + u": the folder is gone"_s;
        }
        return item;
    }
    WallpaperItem item;
    item.id = itemIdFor(path);
    item.source = u"local"_s;
    item.dir = info.absolutePath();
    item.contentDir = item.dir;
    item.title = info.completeBaseName();
    item.kind = ItemKind::Pet;
    // Only the file itself: its folder (Downloads, say) is not the pet's.
    item.risks = scanFileRisk(path);

    const auto animation = readAnimationInfo(path);
    if (!animation) {
        item.status = ItemStatus::Invalid;
        item.statusDetail = info.exists() ? info.fileName() + u": not a readable animation"_s
                                          : info.fileName() + u": the file is gone"_s;
        return item;
    }
    PetVariant variant;
    variant.name = info.completeBaseName();
    variant.file = path;
    variant.width = animation->width;
    variant.height = animation->height;
    variant.fps = animation->fps;
    variant.frames = animation->frames;
    variant.bytes = info.size();
    item.variants.append(std::move(variant));
    return item;
}

LocalPetStore::LocalPetStore(QString filePath, QObject *parent)
    : QObject(parent)
    , m_path(std::move(filePath))
{
}

void LocalPetStore::load()
{
    m_files.clear();
    m_saveBlocked = false;
    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists)
        return;
    QString problem = read.problem;
    if (problem.isEmpty()) {
        if (read.root.value(u"version"_s).toInt() != 1 || !read.root.value(u"files"_s).isArray()) {
            problem = u"unsupported format"_s;
        } else {
            const QJsonArray entries = read.root.value(u"files"_s).toArray();
            if (entries.size() > kMaxLocalPets)
                problem = u"too many files"_s;
            for (const QJsonValue &value : entries) {
                if (!problem.isEmpty())
                    break;
                const QString file = value.toString();
                if (file.isEmpty() || !QFileInfo(file).isAbsolute())
                    problem = u"invalid entry: paths must be absolute strings"_s;
                else if (!m_files.contains(file))
                    m_files.append(file);
            }
        }
    }
    if (problem.isEmpty())
        return;
    m_files.clear();
    if (!quarantineBadFile(m_path, problem, u"no local pets"_s, u"local pets"_s))
        m_saveBlocked = true;
}

bool LocalPetStore::add(const QString &canonicalPath, QString *error)
{
    if (error)
        error->clear();
    if (m_files.contains(canonicalPath))
        return false;
    if (m_files.size() >= kMaxLocalPets)
        return fail(error, u"at most 256 local pets"_s);
    m_files.append(canonicalPath);
    if (!save()) {
        m_files.removeLast();
        return fail(error, u"could not save the local pets"_s);
    }
    emit changed();
    return true;
}

std::optional<QString> LocalPetStore::remove(const QString &itemId, QString *error)
{
    if (error)
        error->clear();
    for (qsizetype i = 0; i < m_files.size(); ++i) {
        if ((m_idOf ? m_idOf(m_files.at(i)) : LocalPetImporter::itemIdFor(m_files.at(i))) != itemId)
            continue;
        const QString file = m_files.takeAt(i);
        if (!save()) {
            m_files.insert(i, file);
            fail(error, u"could not save the local pets"_s);
            return std::nullopt;
        }
        emit changed();
        return file;
    }
    return std::nullopt;
}

bool LocalPetStore::save() const
{
    if (m_saveBlocked) {
        qWarning("murasakid: not saving local pets over the unreadable %s", qPrintable(m_path));
        return false;
    }
    return writeJsonObjectFile(m_path, QJsonObject{{u"version"_s, 1}, {u"files"_s, QJsonArray::fromStringList(m_files)}});
}

} // namespace murasaki
