#include "media/artdecode.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <algorithm>

#include <fcntl.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace murasaki::media {

std::optional<Cover> decodeArt(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > kMaxArtBytes) {
        return std::nullopt;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    // only the plain raster formats of covers: not the exotic ones (PSD, XPM, TGA, ICO, ...) whose readers are far more code to trust
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "webp" && format != "gif") {
        return std::nullopt;
    }
    // The size is read from the header and judged before a pixel is decoded; a picture that does not say it is refused (the formats of
    // the list all do). QImageReader's allocation limit is global to the process and is left alone.
    const QSize size = reader.size();
    if (!size.isValid() || size.width() < 1 || size.height() < 1 || size.width() > kMaxArtSide || size.height() > kMaxArtSide
        || qint64{size.width()} * size.height() > kMaxArtPixels) {
        return std::nullopt;
    }
    reader.setAutoTransform(true);
    QImage image = reader.read(); // (a gif gives its first frame)
    if (image.isNull() || image.sizeInBytes() > kMaxArtPixels * 4 || image.width() > kMaxArtSide || image.height() > kMaxArtSide || qint64{image.width()} * image.height() > kMaxArtPixels) {
        return std::nullopt;
    }
    if (image.width() > kThumbnailSide || image.height() > kThumbnailSide) {
        image = image.scaled(kThumbnailSide, kThumbnailSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || image.width() < 1 || image.height() < 1) {
        return std::nullopt;
    }
    Cover cover;
    cover.width = static_cast<uint32_t>(image.width());
    cover.height = static_cast<uint32_t>(image.height());
    cover.rgba.resize(qsizetype{image.width()} * image.height() * 4);
    for (int y = 0; y < image.height(); ++y) {
        memcpy(cover.rgba.data() + qsizetype{y} * image.width() * 4, image.constScanLine(y), static_cast<size_t>(image.width()) * 4);
    }
    return cover;
}

namespace {

// Judged on the open descriptor, which is what is read: the file system it lives on and where /proc says it is (/dev is covered by
// S_ISREG and the path). The path checked before the open can have changed since.
bool descriptorIsAllowed(int fd)
{
    struct statfs fs;
    // procfs and sysfs, and the file systems whose read can hang for as long as a server or a daemon likes (FUSE, NFS, SMB, CIFS, AFS)
    if (::fstatfs(fd, &fs) != 0 || fs.f_type == PROC_SUPER_MAGIC || fs.f_type == SYSFS_MAGIC || fs.f_type == 0x65735546 /* FUSE */
        || fs.f_type == NFS_SUPER_MAGIC || fs.f_type == SMB_SUPER_MAGIC || fs.f_type == 0xFF534D42 /* CIFS */ || fs.f_type == 0xFE534D42 /* SMB2 */
        || fs.f_type == 0x5346414F /* AFS */) {
        return false;
    }
    char link[4096];
    const QByteArray self = "/proc/self/fd/" + QByteArray::number(fd);
    const ssize_t n = ::readlink(self.constData(), link, sizeof(link) - 1);
    return n > 0 && !isForbiddenArtPath(QString::fromLocal8Bit(link, static_cast<qsizetype>(n)));
}

} // namespace

std::optional<Cover> loadArtFile(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty() || isForbiddenArtPath(canonical)) {
        return std::nullopt;
    }
    const int fd = ::open(QFile::encodeName(canonical).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    struct stat st;
    QByteArray bytes;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= kMaxArtBytes && descriptorIsAllowed(fd);
    if (ok) {
        bytes.resize(static_cast<qsizetype>(st.st_size));
        qsizetype done = 0;
        while (done < bytes.size()) {
            const ssize_t n = ::read(fd, bytes.data() + done, static_cast<size_t>(bytes.size() - done));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            done += static_cast<qsizetype>(n);
        }
        ok = done == bytes.size();
    }
    ::close(fd);
    return ok ? decodeArt(bytes) : std::nullopt;
}

} // namespace murasaki::media
