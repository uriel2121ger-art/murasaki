#include "spritemask.h"

#include "spritedecoder.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace murasaki {

QString defaultMaskCacheRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
}

QString maskCacheSafeName(const QString &name)
{
    QString out;
    for (const QChar c : name.left(80)) {
        const bool ok = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'.' || c == u'_' || c == u'-';
        out += ok ? c : u'_';
    }
    if (out.isEmpty() || out.startsWith(u'.'))
        out.replace(0, out.isEmpty() ? 0 : 1, u"_"_s);
    return out.isEmpty() ? u"_"_s : out;
}

QString maskCacheHash(const QString &name)
{
    return QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
}

QString maskCacheKey(const QString &file)
{
    const QFileInfo info(file);
    if (!info.isFile())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(info.canonicalFilePath().toUtf8());
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    return QString::fromLatin1(hash.result().toHex().left(16));
}

QString maskCachePath(const QString &cacheRoot, const QString &item, const QString &variant, const QString &key)
{
    return cacheRoot + u"/murasaki/pets/"_s + maskCacheSafeName(item) + u'-' + maskCacheHash(item) + u'/' + maskCacheSafeName(variant) + u'-'
        + maskCacheHash(variant) + u'-' + maskCacheSafeName(key) + u".mask"_s;
}

MaskLoad loadMaskPass(const QString &cacheRoot, const QString &item, const QString &variant, const QString &file,
                      const std::atomic_bool *cancel, std::size_t budgetBytes)
{
    MaskLoad out;
    if (cacheRoot.isEmpty()) {
        out.error = u"no cache directory"_s;
        return out;
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        out.cancelled = true;
        return out;
    }
    const QString key = maskCacheKey(file);
    if (key.isEmpty()) {
        out.error = u"cannot read the file"_s;
        return out;
    }
    const QSize native = SpriteDecoder::nativeSize(file);
    out.size = SpriteDecoder::maskPassSize(native);
    if (!out.size.isValid()) {
        out.error = u"cannot read the size"_s;
        return out;
    }
    const QString path = maskCachePath(cacheRoot, item, variant, key);
    if (QFileInfo::exists(path)) {
        // A mask file that does not match the picture size is not used, whatever its name says. The frame count is not checked: the
        // container header only estimates it for some formats, and the key (path, size, modification time) already names the content.
        if (auto cached = readMaskFile(path.toStdString(), out.size.width(), out.size.height(), 0)) {
            out.ok = true;
            out.fromCache = true;
            out.masks = std::move(*cached);
            out.frames = out.masks.frameCount();
            return out;
        }
        QFile::remove(path); // damaged, stale or of another shape: made again below
    }

    SpriteDecoder::MaskPassResult pass = SpriteDecoder::decodeMasks(file, cancel, budgetBytes);
    out.cancelled = pass.result.cancelled;
    if (!pass.result.ok) {
        out.error = pass.result.error;
        if (!out.cancelled)
            qWarning().noquote() << "mask pass failed for" << file << ":" << out.error;
        return out;
    }
    out.size = pass.size;
    out.frames = pass.result.decodedFrames;
    out.masks = std::move(pass.result.masks);
    out.ok = !out.masks.isNull();
    if (!out.ok) {
        out.error = u"no mask"_s;
        return out;
    }

    // Cache it, and drop the variant's files of other keys (the file changed).
    const QString dir = QFileInfo(path).absolutePath();
    if (QDir().mkpath(dir)) {
        // Exactly `<safe variant>-<hash>-<16 hex>.mask`: a variant whose name extends this one with '-' is not matched.
        const QString own = QFileInfo(path).fileName();
        const QString stem = maskCacheSafeName(variant) + u'-' + maskCacheHash(variant);
        const QRegularExpression shape(u"^"_s + QRegularExpression::escape(stem) + u"-[0-9a-f]{16}\\.mask$"_s);
        for (const QString &name : QDir(dir).entryList(QDir::Files)) {
            if (name != own && shape.match(name).hasMatch())
                QFile::remove(QDir(dir).filePath(name));
        }
        writeMaskFile(path.toStdString(), out.masks);
    }
    return out;
}

} // namespace murasaki
