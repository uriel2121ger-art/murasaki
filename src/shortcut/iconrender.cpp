#include "shortcut/iconrender.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QPainter>
#ifdef MURASAKI_SHORTCUT_SVG
#include <QSvgRenderer>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace murasaki::shortcut {

QStringList iconRoots()
{
    QStringList roots;
    const auto add = [&](const QString &dir) {
        const QString canonical = QFileInfo(dir).canonicalFilePath();
        if (!canonical.isEmpty() && !roots.contains(canonical)) {
            roots << canonical;
        }
    };
    const QString dataHome = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    const QString home = dataHome.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : dataHome;
    add(home + QStringLiteral("/icons"));
    add(home + QStringLiteral("/pixmaps"));
    QString dirs = QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS"));
    if (dirs.isEmpty()) {
        dirs = QStringLiteral("/usr/local/share:/usr/share");
    }
    for (const QString &d : dirs.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        add(d + QStringLiteral("/icons"));
        add(d + QStringLiteral("/pixmaps"));
    }
    add(QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/icons"));
    add(QStringLiteral("/var/lib/flatpak/exports/share/icons"));
    add(QStringLiteral("/var/lib/snapd/desktop/icons"));
    // an application's own files (Flatpak and Snap keep icons under their app directories, which the exports link into)
    add(QDir::homePath() + QStringLiteral("/.local/share/flatpak/app"));
    add(QStringLiteral("/var/lib/flatpak/app"));
    add(QStringLiteral("/var/lib/flatpak/exports"));
    add(QStringLiteral("/snap"));
    return roots;
}

bool isIconPath(const QString &spec)
{
    return QDir::isAbsolutePath(spec);
}

std::optional<QImage> loadIconFile(const QString &path, const QStringList &roots)
{
    const QString real = QFileInfo(path).canonicalFilePath();
    if (real.isEmpty()) {
        return std::nullopt;
    }
    const bool inside = std::any_of(roots.begin(), roots.end(), [&](const QString &root) { return real == root || real.startsWith(root + QLatin1Char('/')); });
    if (!inside) {
        return std::nullopt;
    }
    // opened and judged on the descriptor: no FIFO or device can stall the read, and the size is the one that is read
    const int fd = ::open(QFile::encodeName(real).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        return std::nullopt;
    }
    struct stat st;
    QByteArray bytes;
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= kMaxIconFileBytes) {
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
            done += n;
        }
        if (done != bytes.size()) {
            bytes.clear();
        }
    }
    ::close(fd);
    if (bytes.isEmpty()) {
        return std::nullopt;
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAllocationLimit(128); // MiB
    const QSize size = reader.size(); // from the header, before any pixel is decoded
    // only the plain raster formats, as the media covers (src/media/artdecode.cpp): no SVG, no XPM, nothing a plugin would parse
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "webp" && format != "gif") {
        return std::nullopt;
    }
    if (!size.isValid() || size.width() > kMaxIconSourceSide || size.height() > kMaxIconSourceSide
        || static_cast<qint64>(size.width()) * size.height() > kMaxIconSourcePixels) {
        return std::nullopt;
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return std::nullopt;
    }
    return image;
}

namespace {

// "Inherits=a,b" of <dir>/index.theme (a file of at most 64 KiB, the first Icon Theme group only)
QStringList inheritsOf(const QString &themeDir)
{
    QFile file(themeDir + QStringLiteral("/index.theme"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        return {};
    }
    const QList<QByteArray> lines = file.read(64 * 1024).split('\n');
    bool inTheme = false;
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith('[')) {
            inTheme = line == "[Icon Theme]";
        } else if (inTheme && line.startsWith("Inherits=")) {
            QStringList out;
            for (const QByteArray &part : line.mid(9).split(',')) {
                const QString name = QString::fromUtf8(part.trimmed());
                if (!name.isEmpty() && !name.contains(QLatin1Char('/')) && !name.startsWith(QLatin1Char('.'))) {
                    out << name;
                }
            }
            return out;
        }
    }
    return {};
}

bool plainName(const QString &name)
{
    return !name.isEmpty() && name.toUtf8().size() <= 256 && !name.contains(QLatin1Char('/')) && !name.startsWith(QLatin1Char('.'))
        && std::none_of(name.begin(), name.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
}

} // namespace

std::optional<QString> findThemeIcon(const QString &name, const QString &theme, const QStringList &searchPaths)
{
    if (!plainName(name)) {
        return std::nullopt;
    }
    // the themes to look in: the current one, what it inherits (breadth first, four levels at most), then hicolor
    QStringList themes;
    if (plainName(theme)) {
        themes << theme;
    }
    for (int level = 0; level < 4; ++level) {
        QStringList next;
        for (const QString &t : std::as_const(themes)) {
            for (const QString &root : searchPaths) {
                for (const QString &inherited : inheritsOf(root + QLatin1Char('/') + t)) {
                    if (!themes.contains(inherited) && !next.contains(inherited)) {
                        next << inherited;
                    }
                }
            }
        }
        themes << next;
        if (next.isEmpty() || themes.size() > 16) {
            break;
        }
    }
    if (!themes.contains(QStringLiteral("hicolor"))) {
        themes << QStringLiteral("hicolor");
    }
    static const int sizes[] = {512, 256, 128, 96, 64, 48, 32};
    QStringList extensions = {QStringLiteral("png")};
#ifdef MURASAKI_SHORTCUT_SVG
    extensions << QStringLiteral("svg");
#endif
    for (const QString &t : std::as_const(themes)) {
        for (const QString &root : searchPaths) {
            const QString base = root + QLatin1Char('/') + t;
            // the biggest raster first, the vector (any size) after; layouts "<size>x<size>/apps" and "apps/<size>" and "scalable/apps"
            QStringList dirs;
            for (const int size : sizes) {
                dirs << QStringLiteral("%1/%2x%2/apps").arg(base).arg(size) << QStringLiteral("%1/apps/%2").arg(base).arg(size);
            }
            dirs << base + QStringLiteral("/scalable/apps") << base + QStringLiteral("/apps/scalable");
            for (const QString &extension : extensions) {
                for (const QString &dir : std::as_const(dirs)) {
                    const QString path = dir + QLatin1Char('/') + name + QLatin1Char('.') + extension;
                    if (QFileInfo(path).isFile()) {
                        return path;
                    }
                }
            }
        }
    }
    for (const QString &root : searchPaths) { // /usr/share/pixmaps sits beside the icons directory
        for (const QString &extension : extensions) {
            const QString path = QFileInfo(root).absolutePath() + QStringLiteral("/pixmaps/") + name + QLatin1Char('.') + extension;
            if (QFileInfo(path).isFile()) {
                return path;
            }
        }
    }
    return std::nullopt;
}

std::optional<QImage> loadThemeIconFile(const QString &path, const QStringList &roots)
{
    if (path.endsWith(QLatin1String(".svg"))) {
#ifdef MURASAKI_SHORTCUT_SVG
        const QString real = QFileInfo(path).canonicalFilePath();
        const bool inside = std::any_of(roots.begin(), roots.end(), [&](const QString &root) { return real.startsWith(root + QLatin1Char('/')); });
        QFile file(real);
        if (real.isEmpty() || !inside || !file.open(QIODevice::ReadOnly) || file.size() > kMaxIconFileBytes) {
            return std::nullopt;
        }
        QSvgRenderer renderer(file.read(kMaxIconFileBytes));
        if (!renderer.isValid() || !renderer.defaultSize().isValid()) {
            return std::nullopt;
        }
        QImage image(kIconSide, kIconSide, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        const QSizeF size = renderer.defaultSize();
        const qreal scale = std::min(kIconSide / size.width(), kIconSide / size.height());
        painter.translate((kIconSide - size.width() * scale) / 2, (kIconSide - size.height() * scale) / 2);
        painter.scale(scale, scale);
        renderer.render(&painter, QRectF(QPointF(0, 0), size));
        return image;
#else
        return std::nullopt;
#endif
    }
    return loadIconFile(path, roots);
}

std::optional<QByteArray> fitIcon(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return std::nullopt;
    }
    const QImage scaled = image.scaled(kIconSide, kIconSide, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage canvas(kIconSide, kIconSide, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage((kIconSide - scaled.width()) / 2, (kIconSide - scaled.height()) / 2, scaled);
    }
    const QImage out = canvas.convertToFormat(QImage::Format_RGBA8888); // straight alpha
    QByteArray bytes(static_cast<qsizetype>(kIconSide) * kIconSide * 4, '\0');
    for (int y = 0; y < kIconSide; ++y) {
        std::copy_n(out.constScanLine(y), kIconSide * 4, reinterpret_cast<uchar *>(bytes.data()) + static_cast<qsizetype>(y) * kIconSide * 4);
    }
    return bytes;
}

} // namespace murasaki::shortcut
