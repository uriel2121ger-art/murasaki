#include "importjob.h"

#include "animationinfo.h"
#include "apngwriter.h"
#include "boundedzip.h"
#include "colorkey.h"
#include "imagesequence.h"
#include "importfolders.h"
#include "mdaaimport.h"
#include "imagesniff.h"
#include "petimport.h"
#include "sheetslicer.h"
#include "sourcefile.h"
#include "keyedsink.h"
#include "videosource.h"
#include "webpwriter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QLockFile>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStorageInfo>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>

namespace murasaki::import {

namespace {

constexpr long long kMaxSequenceBytes = 4LL << 30; // all the files of an image sequence
constexpr int kSheetAllocationMiB = 512;           // kMaxSheetPixels * 4 bytes, rounded up
constexpr qsizetype kHashBuffer = 64 * 1024;

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

QString shortName(const QString &path)
{
    return QFileInfo(path).fileName().left(80);
}

// A source that must be a regular file within the size limit (a symlink is followed once by QFileInfo and the target must be regular).
bool checkRegularFile(const QString &path, long long *size, QString *error)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        *error = named(kSourceNotFound, shortName(path));
        return false;
    }
    if (!info.isFile()) {
        *error = named(kSourceNotRegular, shortName(path));
        return false;
    }
    if (info.size() > kMaxSourceBytes) {
        *error = named(kSourceOverLimit, shortName(path));
        return false;
    }
    *size = info.size();
    return true;
}

std::optional<QString> sha256File(const QString &path, const std::function<bool()> &cancelled)
{
    QFile file(path);
    if (!openSource(file))
        return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kHashBuffer, 0);
    while (!file.atEnd()) {
        if (cancelled && cancelled())
            return std::nullopt;
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0)
            return std::nullopt;
        hash.addData(QByteArrayView(buffer.constData(), n));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool hasProgramName(const QString &name)
{
    static const QStringList extensions{QStringLiteral(".exe"), QStringLiteral(".dll"), QStringLiteral(".so"), QStringLiteral(".sh"),
                                        QStringLiteral(".bat"), QStringLiteral(".cmd"), QStringLiteral(".msi"), QStringLiteral(".com"), QStringLiteral(".scr")};
    return std::any_of(extensions.begin(), extensions.end(), [&](const QString &e) { return name.endsWith(e, Qt::CaseInsensitive); });
}

// One image of a sequence: a file on disk or a member of the archive.
struct Item {
    QString name;
    QString path;
    int zipIndex = -1;
};

// An item opened for reading, with the format its first bytes say.
struct OpenImage {
    QByteArray bytes; // a zip member
    QBuffer buffer;
    QFile file;
    QIODevice *device = nullptr;
    const char *format = nullptr;
};

QImage::Format pixelFormat()
{
    return QImage::Format_RGBA8888; // straight alpha, the APNG's
}

// Reads a frame: format fixed from the bytes, never from the extension, allocation bounded by QImageReader.
QImage readImage(OpenImage &open, QString *error)
{
    QImageReader reader(open.device, open.format);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(false);
    QImage image = reader.read();
    if (image.isNull()) {
        *error = named(kNotAnImage, reader.errorString().left(120));
        return {};
    }
    return image.convertToFormat(pixelFormat());
}

bool sideOk(int v)
{
    return v >= 1 && v <= kMaxCanvasSide;
}

} // namespace

QString importId(const QByteArray &canonicalSpec, const QStringList &sourceSha256)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(canonicalSpec);
    for (const QString &s : sourceSha256) {
        hash.addData(QByteArrayView("\n"));
        hash.addData(s.toLatin1());
    }
    return QString::fromLatin1(hash.result().toHex().left(24));
}

namespace {

// Hashes every frame on its way to the file (what a test decodes the file against). When the source begins the sink itself (a video does, with the size it
// decoded) the job has already begun the writer: that begin is only checked against it.
class HashingSink final : public FrameSink
{
public:
    HashingSink(FrameSink &target, QCryptographicHash *hash, int width, int height) : m_target(target), m_hash(hash), m_width(width), m_height(height) {}
    bool begin(int width, int height) override
    {
        if (width != m_width || height != m_height) {
            m_error = QStringLiteral("the video's size changed from %1 x %2 to %3 x %4").arg(m_width).arg(m_height).arg(width).arg(height);
            return false;
        }
        return true;
    }
    bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) override
    {
        // The work is bounded by what is really decoded, not by the container's claim of its length.
        m_pixels += (long long)m_width * m_height;
        if (m_pixels > kMaxTotalPixels) {
            m_error = QStringLiteral("more than %1 pixels of frames").arg(kMaxTotalPixels);
            m_overPixels = true;
            return false;
        }
        for (int y = 0; y < m_height; ++y)
            m_hash->addData(QByteArrayView(reinterpret_cast<const char *>(rgba + stride * y), qsizetype(m_width) * 4));
        return m_target.addFrame(rgba, stride, delayMs);
    }
    bool finish() override { return m_target.finish(); }
    int frames() const override { return m_target.frames(); }
    long long bytesWritten() const override { return m_target.bytesWritten(); }
    QString error() const override { return m_error.isEmpty() ? m_target.error() : m_error; }
    bool overBudget() const override { return m_overPixels || m_target.overBudget(); }

private:
    long long m_pixels = 0;
    bool m_overPixels = false;
    FrameSink &m_target;
    QCryptographicHash *m_hash;
    int m_width;
    int m_height;
    QString m_error;
};

// The shared front half of preview and run: finds the sources, checks them, reads the headers, makes the plan.
class Job
{
public:
    Job(const ImportSpec &spec, const ImportOptions &options) : m_spec(spec), m_options(options) { m_clock.start(); }

    // Headers and checks. False with error() set.
    bool prepare()
    {
        // The same allocation limit for the preview and for the run, so that what looks fine fits.
        QImageReader::setAllocationLimit(m_spec.kind == ImportKind::Sheet ? kSheetAllocationMiB : int(kMaxImageAllocationMiB));
        if (!prepareKind())
            return false;
        if ((long long)m_frames * m_width * m_height > kMaxTotalPixels)
            return fail(named(kOutputOverBudget, QStringLiteral("%1 frames of %2 x %3 px is more than %4 pixels").arg(m_frames).arg(m_width).arg(m_height).arg(kMaxTotalPixels)));
        return true;
    }

    bool prepareKind()
    {
        switch (m_spec.kind) {
        case ImportKind::Sheet:
            return prepareSheet();
        case ImportKind::Images:
            return gatherFiles() && planItems();
        case ImportKind::Zip:
            return gatherZip() && planItems();
        case ImportKind::Gif:
            return prepareGif();
        case ImportKind::Video:
            return prepareVideo();
        case ImportKind::Mdaa:
            return false; // a package is not rendered frame by frame: runMdaaImport and previewImport handle it
        }
        return false;
    }

    const QString &error() const { return m_error; }
    int frames() const { return m_frames; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    long long totalDelayMs() const { return m_spec.kind == ImportKind::Gif || m_spec.kind == ImportKind::Video ? -1 : (long long)m_frames * effectiveFrameMs(m_spec); }
    const QStringList &degraded() const { return m_degraded; }
    bool sinkFinished() const { return m_finished; }
    bool hasAudio() const { return m_hasAudio; }
    QStringList sourcePaths() const { return m_files; }

    // Decodes and writes every frame to `writer`; fills the totals. False with error() set.
    bool render(FrameSink *writer, QCryptographicHash *frameHash, long long *totalDelay, const std::function<void(int)> &step)
    {
        m_writer = writer;
        m_frameHash = frameHash;
        m_totalDelay = totalDelay;
        m_step = step;
        switch (m_spec.kind) {
        case ImportKind::Sheet:
            return renderSheet();
        case ImportKind::Images:
        case ImportKind::Zip:
            return renderItems();
        case ImportKind::Gif:
            return renderGif();
        case ImportKind::Video:
            return renderVideo();
        case ImportKind::Mdaa:
            return false;
        }
        return false;
    }

private:
    bool fail(const QString &message)
    {
        m_error = message;
        return false;
    }

    bool cancelled() const { return m_options.cancelled && m_options.cancelled(); }
    // Why the work must stop now (cancelled, or out of time), or an empty string.
    QString interrupted() const
    {
        if (cancelled())
            return named(kCancelled);
        if (m_options.maxMillis > 0 && m_clock.elapsed() > m_options.maxMillis)
            return named(kTimeBudget);
        return {};
    }

    // Opens an item for reading and sniffs it.
    std::unique_ptr<OpenImage> open(const Item &item)
    {
        auto o = std::make_unique<OpenImage>();
        if (item.zipIndex >= 0) {
            QString why;
            const auto bytes = m_zip->read(item.zipIndex, &why);
            if (!bytes) {
                fail(why);
                return nullptr;
            }
            o->bytes = *bytes;
            o->buffer.setData(o->bytes);
            o->buffer.open(QIODevice::ReadOnly);
            o->device = &o->buffer;
        } else {
            o->file.setFileName(item.path);
            if (!openSource(o->file)) {
                fail(named(kSourceNotFound, shortName(item.path)));
                return nullptr;
            }
            o->device = &o->file;
        }
        const QByteArray head = o->device->peek(16);
        o->format = sniffImageFormat(head);
        if (!o->format) {
            fail(named(kNotAnImage, item.name.left(80)));
            return nullptr;
        }
        return o;
    }

    // ---- sheet

    bool prepareSheet()
    {
        long long size = 0;
        const QString &path = m_spec.sources.first();
        if (!checkRegularFile(path, &size, &m_error))
            return false;
        m_files = {path};
        QFile file(path);
        if (!openSource(file))
            return fail(named(kSourceNotFound, shortName(path)));
        const QByteArray head = file.peek(24);
        const char *format = sniffImageFormat(head);
        if (!format)
            return fail(named(kNotAnImage, shortName(path)));
        QImageReader reader(&file, format);
        reader.setDecideFormatFromContent(false);
        QSize s = reader.size();
        if (!s.isValid())
            s = pngHeaderSize(head); // the decoder gives no size to an image over its allocation limit
        if (!s.isValid())
            return fail(named(kNotAnImage, QStringLiteral("%1: the header has no size").arg(shortName(path))));
        m_sheet = m_spec.sheet;
        m_sheet.sheetWidth = s.width();
        m_sheet.sheetHeight = s.height();
        m_layout = sliceSheet(m_sheet);
        if (!m_layout.ok())
            return fail(m_layout.error);
        m_frames = int(m_layout.cells.size());
        m_width = m_sheet.frameWidth;
        m_height = m_sheet.frameHeight;
        return true;
    }

    bool renderSheet()
    {
        QImageReader::setAllocationLimit(kSheetAllocationMiB);
        QFile file(m_spec.sources.first());
        if (!openSource(file))
            return fail(named(kSourceNotFound, shortName(m_spec.sources.first())));
        const char *format = sniffImageFormat(file.peek(16));
        QImageReader reader(&file, format);
        reader.setDecideFormatFromContent(false);
        reader.setAutoTransform(false);
        const QImage sheet = reader.read().convertToFormat(pixelFormat());
        if (sheet.isNull() || sheet.width() != m_sheet.sheetWidth || sheet.height() != m_sheet.sheetHeight)
            return fail(named(kNotAnImage, reader.errorString().left(120)));
        for (int i = 0; i < m_layout.cells.size(); ++i) {
            if (const QString why = interrupted(); !why.isEmpty())
                return fail(why);
            const SheetCell &c = m_layout.cells.at(i);
            const QImage cell = sheet.copy(c.x, c.y, m_sheet.frameWidth, m_sheet.frameHeight);
            if (!put(cell, effectiveFrameMs(m_spec)))
                return false;
            m_step(i + 1);
        }
        return true;
    }

    // ---- sequence of files or members

    bool gatherFiles()
    {
        QStringList paths;
        if (m_spec.sources.size() == 1 && QFileInfo(m_spec.sources.first()).isDir()) {
            const QDir dir(m_spec.sources.first());
            QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
            std::sort(names.begin(), names.end(), naturalLess);
            if (names.size() > kMaxSources)
                return fail(named(kImagesTooMany, QStringLiteral("%1 files").arg(names.size())));
            // A folder often holds a Thumbs.db or a README: what does not start like an image is skipped and counted (as a ZIP's other members are).
            int skipped = 0;
            for (const QString &n : names) {
                QFile probe(dir.filePath(n));
                if (openSource(probe) && sniffImageFormat(probe.read(16)))
                    paths.append(dir.filePath(n));
                else
                    ++skipped;
            }
            if (skipped > 0)
                m_degraded.append(QStringLiteral("images: %1 files ignored").arg(skipped));
        } else {
            paths = m_spec.sources;
            std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) { return naturalLess(QFileInfo(a).fileName(), QFileInfo(b).fileName()) || (QFileInfo(a).fileName() == QFileInfo(b).fileName() && a < b); });
        }
        long long total = 0;
        for (const QString &p : paths) {
            long long size = 0;
            if (!checkRegularFile(p, &size, &m_error))
                return false;
            total += size;
            if (total > kMaxSequenceBytes)
                return fail(named(kSourceOverLimit, QStringLiteral("the files together pass %1 bytes").arg(kMaxSequenceBytes)));
            m_items.append({QFileInfo(p).fileName(), p, -1});
        }
        m_files = paths;
        return true;
    }

    bool gatherZip()
    {
        long long size = 0;
        const QString &path = m_spec.sources.first();
        if (!checkRegularFile(path, &size, &m_error))
            return false;
        m_files = {path};
        m_zipFile.setFileName(path);
        if (!openSource(m_zipFile))
            return fail(named(kSourceNotFound, shortName(path)));
        QString why;
        ZipLimits limits;
        limits.maxMemberBytes = 64LL << 20; // one frame
        limits.skipOversize = true;         // a big video or a repetitive text file beside the frames is ignored, not a reason to refuse the archive
        m_zip = BoundedZip::open(&m_zipFile, limits, &why);
        if (!m_zip)
            return fail(why);
        QList<std::pair<QString, int>> images;
        int ignored = 0;
        for (int i = 0; i < m_zip->entries().size(); ++i) {
            const ZipEntry &e = m_zip->entries().at(i);
            if (e.isDirectory)
                continue;
            if (e.tooBig) {
                ++ignored;
                continue;
            }
            const bool image = e.size >= 16 && !hasProgramName(e.name);
            if (!image) {
                ++ignored;
                if (hasProgramName(e.name))
                    m_degraded.append(QStringLiteral("program not run: %1").arg(e.name.left(80)));
                continue;
            }
            images.append({e.name, i});
        }
        std::sort(images.begin(), images.end(), [](const auto &a, const auto &b) { return naturalLess(a.first, b.first) || (a.first == b.first && a.second < b.second); });
        // An entry whose bytes are not an image is ignored too, found when its first bytes are looked at (headers pass).
        QList<Item> items;
        for (const auto &[name, index] : images) {
            if (items.size() > kMaxSources)
                return fail(named(kImagesTooMany));
            items.append({name, QString(), index});
        }
        m_items = items;
        m_zipIgnored = ignored;
        return true;
    }

    bool planItems()
    {
        QList<ImageDim> dims;
        QList<Item> kept;
        int notImages = 0;
        for (const Item &item : std::as_const(m_items)) {
            if (const QString why = interrupted(); !why.isEmpty())
                return fail(why);
            auto o = open(item);
            if (!o) {
                if (m_spec.kind == ImportKind::Zip && m_error.startsWith(named(kNotAnImage))) {
                    ++notImages;
                    m_error.clear();
                    continue;
                }
                return false;
            }
            const QByteArray head24 = o->device->peek(24); // before the reader moves the device: a PNG's own header names a size the decoder will not give
            QImageReader reader(o->device, o->format);
            reader.setDecideFormatFromContent(false);
            QSize s = reader.size();
            if (!s.isValid())
                s = pngHeaderSize(head24);
            if (!s.isValid())
                return fail(named(kNotAnImage, QStringLiteral("%1: the header has no size").arg(item.name.left(80))));
            if (!sideOk(s.width()) || !sideOk(s.height()))
                return fail(named(kNotAnImage, QStringLiteral("%1 is %2 x %3").arg(item.name.left(80)).arg(s.width()).arg(s.height())));
            dims.append({s.width(), s.height()});
            kept.append(item);
        }
        if (m_spec.kind == ImportKind::Zip && kept.isEmpty())
            return fail(named(kZipNoImages));
        m_items = kept;
        if (m_zipIgnored + notImages > 0)
            m_degraded.prepend(QStringLiteral("zip: %1 entries ignored").arg(m_zipIgnored + notImages));
        m_plan = planSequence(dims, m_spec.sizePolicy);
        if (!m_plan.ok())
            return fail(m_plan.error);
        m_frames = int(m_items.size());
        m_width = m_plan.canvasWidth;
        m_height = m_plan.canvasHeight;
        if (m_spec.sizePolicy == SizePolicy::Scale
            && std::any_of(dims.begin(), dims.end(), [&](const ImageDim &d) { return d.width != m_width || d.height != m_height; }))
            m_degraded.append(named(kScaledDown));
        return true;
    }

    bool renderItems()
    {
        QImageReader::setAllocationLimit(int(kMaxImageAllocationMiB));
        for (int i = 0; i < m_items.size(); ++i) {
            if (const QString why = interrupted(); !why.isEmpty())
                return fail(why);
            auto o = open(m_items.at(i));
            if (!o)
                return false;
            QImage image = readImage(*o, &m_error);
            if (image.isNull())
                return false;
            const Placement &p = m_plan.placements.at(i);
            QImage canvas(m_width, m_height, pixelFormat());
            canvas.fill(Qt::transparent);
            if (p.width != image.width() || p.height != image.height()) {
                image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).scaled(p.width, p.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(pixelFormat());
            }
            for (int y = 0; y < image.height(); ++y)
                std::memcpy(canvas.scanLine(p.y + y) + p.x * 4, image.constScanLine(y), size_t(image.width()) * 4);
            if (!put(canvas, effectiveFrameMs(m_spec)))
                return false;
            m_step(i + 1);
        }
        return true;
    }

    // ---- gif

    bool prepareGif()
    {
        long long size = 0;
        const QString &path = m_spec.sources.first();
        if (!checkRegularFile(path, &size, &m_error))
            return false;
        m_files = {path};
        QFile file(path);
        if (!openSource(file))
            return fail(named(kSourceNotFound, shortName(path)));
        const QByteArray head = file.peek(16);
        const char *format = sniffImageFormat(head);
        if (!format || qstrcmp(format, "gif") != 0)
            return fail(named(kNotAnImage, shortName(path)));
        QImageReader reader(&file, format);
        reader.setDecideFormatFromContent(false);
        const QSize s = reader.size();
        if (!s.isValid() || !sideOk(s.width()) || !sideOk(s.height()))
            return fail(named(kNotAnImage, QStringLiteral("%1: size not usable").arg(shortName(path))));
        const int count = reader.imageCount();
        if (count < 2)
            return fail(named(kNotAnImage, QStringLiteral("%1: not animated").arg(shortName(path))));
        if (count > kMaxFrames)
            return fail(named(kTooManyFrames, QString::number(count)));
        m_frames = count;
        m_width = s.width();
        m_height = s.height();
        return true;
    }

    bool renderGif()
    {
        QImageReader::setAllocationLimit(int(kMaxImageAllocationMiB));
        QFile file(m_spec.sources.first());
        if (!openSource(file))
            return fail(named(kSourceNotFound, shortName(m_spec.sources.first())));
        QImageReader reader(&file, "gif");
        reader.setDecideFormatFromContent(false);
        reader.setAutoTransform(false);
        int n = 0;
        while (reader.canRead()) {
            if (const QString why = interrupted(); !why.isEmpty())
                return fail(why);
            if (n >= kMaxFrames)
                return fail(named(kTooManyFrames));
            const QImage raw = reader.read();
            if (raw.isNull())
                return fail(named(kNotAnImage, reader.errorString().left(120)));
            const int delay = qMax(0, reader.nextImageDelay());
            const QImage image = raw.convertToFormat(pixelFormat());
            if (image.width() != m_width || image.height() != m_height)
                return fail(named(kNotAnImage, QStringLiteral("gif: the frame size changes")));
            if (!put(image, delay))
                return false;
            m_step(++n);
        }
        m_frames = n;
        return true;
    }

    // ---- video

    VideoSettings videoSettings() const
    {
        VideoSettings v;
        v.maxSide = m_spec.maxSide;
        v.maxFps = m_spec.maxFps;
        v.startMs = m_spec.startMs;
        v.durationMs = m_spec.durationMs;
        const long long left = m_options.maxMillis > 0 ? m_options.maxMillis - m_clock.elapsed() : v.deadlineMs;
        v.deadlineMs = qBound(kMinDeadlineMs, left, kMaxDeadlineMs);
        return v;
    }

    bool prepareVideo()
    {
        long long size = 0;
        const QString &path = m_spec.sources.first();
        if (!checkRegularFile(path, &size, &m_error))
            return false;
        m_files = {path};
        const VideoSettings settings = videoSettings();
        if (const QString why = validateVideoSettings(settings); !why.isEmpty())
            return fail(named(kSpecRefused, why));
        VideoCallbacks callbacks;
        callbacks.cancelled = [this] { return !interrupted().isEmpty(); };
        const VideoInfo info = probeVideo(path, settings, callbacks);
        if (!info.ok)
            return fail(info.error);
        m_frames = info.estimatedFrames; // 0 when the container says nothing: the decode counts for real
        m_width = info.outWidth;
        m_height = info.outHeight;
        m_degraded.append(info.degraded);
        m_hasAudio = info.hasAudio;
        return true;
    }

    bool renderVideo()
    {
        HashingSink hashing(*m_writer, m_frameHash, m_width, m_height);
        KeyedSink keyed(hashing, m_spec.colorKey);
        VideoCallbacks callbacks;
        callbacks.cancelled = [this] { return !interrupted().isEmpty(); };
        callbacks.progress = [this](double, int frames) { m_step(frames); };
        const VideoOutcome outcome = decodeVideo(m_spec.sources.first(), videoSettings(), keyed, callbacks);
        if (!outcome.ok) {
            if (outcome.cancelled) {
                const QString why = interrupted();
                return fail(why.isEmpty() ? named(kCancelled) : why);
            }
            if (outcome.overBudget)
                return fail(named(kOutputOverBudget));
            return fail(outcome.error.isEmpty() ? named(kHelperFailed, hashing.error()) : outcome.error);
        }
        m_finished = true; // (decodeVideo finishes the sink itself)
        m_frames = outcome.frames;
        *m_totalDelay = outcome.totalDelayMs;
        for (const QString &d : outcome.degraded)
            if (!m_degraded.contains(d))
                m_degraded.append(d);
        return true;
    }

    bool put(QImage image, int delayMs)
    {
        if (m_spec.colorKey.enabled) {
            image.detach();
            applyColorKey(image.bits(), image.width(), image.height(), image.bytesPerLine(), m_spec.colorKey);
        }
        for (int y = 0; y < image.height(); ++y)
            m_frameHash->addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)), qsizetype(image.width()) * 4));
        if (!m_writer->addFrame(image.constBits(), image.bytesPerLine(), delayMs))
            return fail(m_writer->overBudget() ? named(kOutputOverBudget) : named(kHelperFailed, m_writer->error()));
        *m_totalDelay += delayMs;
        return true;
    }

    const ImportSpec &m_spec;
    const ImportOptions &m_options;
    QElapsedTimer m_clock;
    bool m_finished = false;
    bool m_hasAudio = false;
    QString m_error;
    QStringList m_degraded;
    QStringList m_files;
    int m_frames = 0;
    int m_width = 0;
    int m_height = 0;
    SheetSpec m_sheet;
    SheetLayout m_layout;
    QList<Item> m_items;
    SequencePlan m_plan;
    int m_zipIgnored = 0;
    QFile m_zipFile;
    std::optional<BoundedZip> m_zip;
    FrameSink *m_writer = nullptr;
    QCryptographicHash *m_frameHash = nullptr;
    long long *m_totalDelay = nullptr;
    std::function<void(int)> m_step;
};

constexpr qint64 kMaxRecordBytes = 8 << 20;

// The result of an earlier run of the same import, if its folder still holds what the record says: the animation reads back with the recorded frame count
// and size, or every animation of the package is there. Anything less is not reused (the caller makes it again and replaces the folder).
ImportOutcome fromRecord(const QString &directory, const QString &id)
{
    ImportOutcome out;
    QFile record(directory + QStringLiteral("/source.json"));
    if (!record.open(QIODevice::ReadOnly) || record.size() > kMaxRecordBytes)
        return out;
    const QJsonObject o = QJsonDocument::fromJson(record.readAll()).object();
    out.id = id;
    out.directory = directory;
    out.reused = true;
    for (const QJsonValue &d : o.value(QLatin1String("degraded")).toArray())
        out.degraded.append(d.toString());
    if (o.contains(QLatin1String("mdaa"))) { // a package: the folder is the pet
        const QJsonArray variants = o.value(QLatin1String("mdaa")).toObject().value(QLatin1String("variants")).toArray();
        for (const QJsonValue &v : variants) {
            const QString name = v.toObject().value(QLatin1String("file")).toString();
            if (name.isEmpty() || name.contains(QLatin1Char('/')) || !readAnimationInfo(directory + QLatin1Char('/') + name))
                return ImportOutcome();
        }
        out.ok = !variants.isEmpty();
        out.file = directory;
        out.variants = int(variants.size());
        out.bytes = o.value(QLatin1String("output")).toObject().value(QLatin1String("bytes")).toInteger();
        return out.ok ? out : ImportOutcome();
    }
    const QJsonObject result = o.value(QLatin1String("output")).toObject();
    const QString file = result.value(QLatin1String("file")).toString();
    if (file.isEmpty() || file.contains(QLatin1Char('/')))
        return ImportOutcome();
    out.file = directory + QLatin1Char('/') + file;
    const auto info = readAnimationInfo(out.file);
    const int frames = result.value(QLatin1String("frames")).toInt();
    if (!info || info->frames != frames || info->width != result.value(QLatin1String("width")).toInt() || info->height != result.value(QLatin1String("height")).toInt())
        return ImportOutcome();
    out.ok = true;
    out.frames = frames;
    out.width = info->width;
    out.height = info->height;
    out.totalDelayMs = result.value(QLatin1String("totalDelayMs")).toInteger();
    out.bytes = result.value(QLatin1String("bytes")).toInteger();
    out.framesSha256 = result.value(QLatin1String("framesSha256")).toString();
    return out;
}

// The sources as the record keeps them: every one up to 200, else the count, the total size, one hash over all their hashes and the first few names, so that
// a 20000-image sequence does not make a record the next run refuses to read.
QJsonValue sourcesRecord(const QStringList &paths, const QStringList &hashes)
{
    if (paths.size() <= 200) {
        QJsonArray sources;
        for (int i = 0; i < paths.size(); ++i)
            sources.append(QJsonObject{{QStringLiteral("name"), shortName(paths.at(i))}, {QStringLiteral("size"), QFileInfo(paths.at(i)).size()}, {QStringLiteral("sha256"), hashes.at(i)}});
        return sources;
    }
    QCryptographicHash all(QCryptographicHash::Sha256);
    qint64 total = 0;
    QJsonArray first;
    for (int i = 0; i < paths.size(); ++i) {
        all.addData(hashes.at(i).toLatin1());
        total += QFileInfo(paths.at(i)).size();
        if (i < 20)
            first.append(shortName(paths.at(i)));
    }
    return QJsonObject{{QStringLiteral("count"), int(paths.size())}, {QStringLiteral("totalSize"), total}, {QStringLiteral("sha256"), QString::fromLatin1(all.result().toHex())}, {QStringLiteral("first"), first}};
}

// Puts the finished work folder in place with one rename. A folder already there is either an equal result another job made first (kept) or a damaged one
// (moved aside, then removed once the new one is in): never a dead end. Returns the outcome of a kept folder in *kept.
bool publishFolder(const QString &partDir, const QString &finalDir, const QString &id, ImportOutcome *kept)
{
    if (QDir().rename(partDir, finalDir))
        return true;
    const ImportOutcome reused = fromRecord(finalDir, id);
    if (reused.ok) {
        *kept = reused;
        QDir(partDir).removeRecursively();
        return true;
    }
    if (!QFileInfo::exists(finalDir))
        return false;
    const QString aside = finalDir + QStringLiteral(".stale");
    QDir(aside).removeRecursively();
    if (!QDir().rename(finalDir, aside) || !QDir().rename(partDir, finalDir))
        return false;
    QDir(aside).removeRecursively();
    return true;
}

bool commitOutput(QFile &out)
{
    const bool flushed = out.flush();
    const bool clean = out.error() == QFileDevice::NoError;
    out.close();
    return flushed && clean && out.error() == QFileDevice::NoError;
}

// A copy of a source into the job folder: opened like every source (non-blocking, a regular file), written in blocks, every error checked, cancellable.
bool copySource(const QString &from, const QString &to, const std::function<bool()> &cancelled)
{
    QFile in(from);
    if (!openSource(in))
        return false;
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    QByteArray block(1 << 20, 0);
    while (!in.atEnd()) {
        if (cancelled && cancelled())
            return false;
        const qint64 n = in.read(block.data(), block.size());
        if (n < 0 || out.write(block.constData(), n) != n)
            return false;
    }
    return commitOutput(out);
}

} // namespace

ImportPreview previewImport(const ImportSpec &spec)
{
    ImportPreview preview;
    if (spec.kind == ImportKind::Mdaa) {
        const MdaaPreview p = previewMdaa(spec.sources.first());
        preview.ok = p.ok();
        preview.error = p.error;
        preview.frames = p.animations;
        preview.degraded = p.degraded;
        return preview;
    }
    ImportOptions none;
    Job job(spec, none);
    if (!job.prepare()) {
        preview.error = job.error();
        return preview;
    }
    preview.ok = true;
    preview.frames = job.frames();
    preview.width = job.width();
    preview.height = job.height();
    preview.totalDelayMs = job.totalDelayMs();
    preview.estimatedBytes = (long long)job.frames() * job.width() * job.height() * 4;
    preview.degraded = job.degraded();
    return preview;
}

namespace {

// The scene a new pet of a package starts in is not part of the import's identity; a reused folder takes the new choice.
void recordAutoScene(const QString &directory, int autoScene)
{
    QFile file(directory + QStringLiteral("/source.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxRecordBytes)
        return;
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject mdaa = root.value(QStringLiteral("mdaa")).toObject();
    const int scenes = int(mdaa.value(QStringLiteral("scenes")).toArray().size());
    const int wanted = autoScene >= 0 && autoScene < scenes ? autoScene : -1;
    if (mdaa.value(QStringLiteral("autoScene")).toInt(-1) == wanted)
        return;
    mdaa.insert(QStringLiteral("autoScene"), wanted);
    root.insert(QStringLiteral("mdaa"), mdaa);
    QSaveFile out(directory + QStringLiteral("/source.json"));
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size())
        out.commit();
}

// A package: the folder holds the animations, the preview and the record; nothing is rendered.
ImportOutcome runMdaaImport(const ImportSpec &spec, const ImportOptions &options)
{
    ImportOutcome outcome;
    const auto fail = [&](const QString &message) {
        outcome.ok = false;
        outcome.error = message;
        return outcome;
    };
    if (options.outRoot.isEmpty() || !QDir().mkpath(options.outRoot))
        return fail(named(kHelperFailed, QStringLiteral("the output folder cannot be made")));
    const QString source = spec.sources.first();
    long long size = 0;
    QString error;
    if (!checkRegularFile(source, &size, &error))
        return fail(error);
    const auto hash = sha256File(source, options.cancelled);
    if (!hash)
        return fail(options.cancelled && options.cancelled() ? named(kCancelled) : named(kSourceNotFound, shortName(source)));
    const QByteArray canonical = canonicalJson(spec);
    const QString id = importId(canonical, {*hash});
    const QString finalDir = QDir(options.outRoot).filePath(id);
    QLockFile lock(QDir(options.outRoot).filePath(QStringLiteral(".") + id + QStringLiteral(".lock")));
    lock.setStaleLockTime(30 * 60 * 1000);
    if (!lock.tryLock(0))
        return fail(named(kHelperFailed, QStringLiteral("the same import is already running")));
    if (QFileInfo(finalDir).isDir()) {
        ImportOutcome reused = fromRecord(finalDir, id);
        if (reused.ok) {
            recordAutoScene(finalDir, spec.autoScene);
            return reused;
        }
    }
    if (QStorageInfo(options.outRoot).bytesAvailable() < qMin(qMax(size * 4, 64LL << 20), options.maxOutputBytes))
        return fail(named(kNotEnoughDisk));
    const QString partDir = finalDir + QStringLiteral(".part");
    if (QFileInfo(partDir).exists())
        QDir(partDir).removeRecursively();
    if (!QDir().mkpath(partDir))
        return fail(named(kHelperFailed, QStringLiteral("the work folder cannot be made")));
    const auto cleanup = [&] { QDir(partDir).removeRecursively(); };

    const MdaaResult result = importMdaa(
        source, partDir, options.cancelled,
        [&](double f) {
            if (options.progress)
                options.progress(f, 0);
        },
        MdaaBudget{options.maxOutputBytes, options.maxMillis});
    if (!result.ok()) {
        cleanup();
        return fail(result.error);
    }
    long long bytes = 0;
    for (const QFileInfo &f : QDir(partDir).entryInfoList(QDir::Files))
        bytes += f.size();
    if (bytes > options.maxOutputBytes) {
        cleanup();
        return fail(named(kOutputOverBudget));
    }
    QJsonObject mdaa = mdaaRecord(result);
    if (spec.autoScene >= 0)
        mdaa.insert(QStringLiteral("autoScene"), spec.autoScene < result.scenes.size() ? spec.autoScene : -1);
    QJsonArray degraded = QJsonArray::fromStringList(result.degraded);
    const QJsonObject record{{QStringLiteral("version"), 1},
                             {QStringLiteral("tool"), QStringLiteral("murasaki-petimport 1")},
                             {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                             {QStringLiteral("spec"), QJsonDocument::fromJson(canonical).object()},
                             {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("name"), shortName(source)}, {QStringLiteral("size"), size}, {QStringLiteral("sha256"), *hash}}}},
                             {QStringLiteral("output"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("mdaa")}, {QStringLiteral("bytes"), bytes}}},
                             {QStringLiteral("degraded"), degraded},
                             {QStringLiteral("mdaa"), mdaa}};
    QSaveFile recordFile(partDir + QStringLiteral("/source.json"));
    const QByteArray recordBytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
    if (!recordFile.open(QIODevice::WriteOnly) || recordFile.write(recordBytes) != recordBytes.size() || !recordFile.commit()) {
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("source.json cannot be written")));
    }
    if (options.cancelled && options.cancelled()) {
        cleanup();
        return fail(named(kCancelled));
    }
    ImportOutcome kept;
    if (!publishFolder(partDir, finalDir, id, &kept)) {
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("the result cannot be moved into place")));
    }
    if (kept.ok)
        return kept;
    outcome.ok = true;
    outcome.id = id;
    outcome.directory = finalDir;
    outcome.file = finalDir;
    outcome.variants = int(result.variants.size());
    outcome.bytes = bytes;
    outcome.degraded = result.degraded;
    return outcome;
}

} // namespace

ImportOutcome runImport(const ImportSpec &spec, const ImportOptions &options)
{
    if (spec.kind == ImportKind::Mdaa)
        return runMdaaImport(spec, options);
    ImportOutcome outcome;
    const auto fail = [&](const QString &message) {
        outcome.ok = false;
        outcome.error = message;
        return outcome;
    };
    if (options.outRoot.isEmpty() || !QDir().mkpath(options.outRoot))
        return fail(named(kHelperFailed, QStringLiteral("the output folder cannot be made")));

    Job job(spec, options);
    if (!job.prepare())
        return fail(job.error());
    if (job.frames() > kMaxFrames)
        return fail(named(kTooManyFrames));

    QStringList hashes;
    for (const QString &path : job.sourcePaths()) {
        const auto h = sha256File(path, options.cancelled);
        if (!h)
            return fail(options.cancelled && options.cancelled() ? named(kCancelled) : named(kSourceNotFound, shortName(path)));
        hashes.append(*h);
    }
    const QByteArray canonical = canonicalJson(spec);
    const QString id = importId(canonical, hashes);
    const QString finalDir = QDir(options.outRoot).filePath(id);
    // Two runs of the same import (a second murasakictl, a retry) would share one work folder: the second waits for the first by failing at once.
    QLockFile lock(QDir(options.outRoot).filePath(QStringLiteral(".") + id + QStringLiteral(".lock")));
    lock.setStaleLockTime(30 * 60 * 1000);
    if (!lock.tryLock(0))
        return fail(named(kHelperFailed, QStringLiteral("the same import is already running")));
    if (QFileInfo(finalDir).isDir()) {
        ImportOutcome reused = fromRecord(finalDir, id);
        if (reused.ok)
            return reused;
    }

    const long long raw = job.frames() > 0 ? (long long)job.frames() * job.width() * job.height() * 4 : options.maxOutputBytes; // (a video may not say how long it is)
    // The audio of a video is kept as a copy of the video, and only when there is an audio stream to keep.
    const bool keepCopy = spec.keepSource || (spec.kind == ImportKind::Video && spec.keepAudio && job.hasAudio());
    long long copyBytes = 0;
    if (keepCopy)
        for (const QString &src : job.sourcePaths())
            copyBytes += QFileInfo(src).size();
    const long long needed = qMin(raw, options.maxOutputBytes) + copyBytes;
    if (QStorageInfo(options.outRoot).bytesAvailable() < needed)
        return fail(named(kNotEnoughDisk));

    const QString partDir = finalDir + QStringLiteral(".part");
    if (QFileInfo(partDir).exists())
        QDir(partDir).removeRecursively();
    if (!QDir().mkpath(partDir))
        return fail(named(kHelperFailed, QStringLiteral("the work folder cannot be made")));
    const auto cleanup = [&] { QDir(partDir).removeRecursively(); };

    const bool webp = spec.format == OutputFormat::Webp;
    if (webp && !WebpWriter::webpOutputAvailable())
        return fail(named(kSpecRefused, QStringLiteral("this build has no animated WebP output")));
    const QString fileName = sanitiseOutputName(spec.name.isEmpty() ? shortName(spec.sources.first()) : spec.name) + (webp ? QStringLiteral(".webp") : QStringLiteral(".apng"));
    QFile out(partDir + QLatin1Char('/') + fileName);
    if (!out.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("the output cannot be created")));
    }
    std::unique_ptr<FrameSink> sink;
    if (webp)
        sink = std::make_unique<WebpWriter>(&out, spec.webpQuality, options.maxOutputBytes);
    else
        sink = std::make_unique<ApngWriter>(&out, options.maxOutputBytes);
    FrameSink &writer = *sink;
    if (!writer.begin(job.width(), job.height())) {
        cleanup();
        return fail(writer.overBudget() ? named(kOutputOverBudget) : named(kHelperFailed, writer.error()));
    }
    QCryptographicHash frameHash(QCryptographicHash::Sha256);
    long long totalDelay = 0;
    const int frames = job.frames();
    const bool rendered = job.render(&writer, &frameHash, &totalDelay, [&](int done) {
        if (options.progress)
            options.progress(frames > 0 && done <= frames ? double(done) / frames : -1.0, done); // (-1: the length is not known, or was understated)
    });
    if (!rendered || (!job.sinkFinished() && !writer.finish())) {
        const QString why = !rendered ? job.error() : (writer.overBudget() ? named(kOutputOverBudget) : named(kHelperFailed, writer.error()));
        out.close();
        cleanup();
        return fail(why.isEmpty() ? named(kHelperFailed) : why);
    }
    const long long bytes = writer.bytesWritten();
    if (!commitOutput(out)) { // a full disk shows here, not in write()
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("the output could not be written completely")));
    }

    // The output must read back as what was written: header, size and frame count.
    QFile check(out.fileName());
    std::optional<AnimationInfo> info;
    if (check.open(QIODevice::ReadOnly))
        info = webp ? readWebpInfo(&check) : readApngInfo(&check);
    check.close();
    // An animated WebP encoder merges identical neighbouring frames (their delays add up), so it may hold fewer frames than were fed.
    const bool framesAgree = info && (webp ? (info->frames >= 1 && info->frames <= writer.frames()) : info->frames == writer.frames());
    if (!framesAgree || info->width != job.width() || info->height != job.height()) {
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("the output did not read back")));
    }

    outcome.id = id;
    outcome.directory = finalDir;
    outcome.file = finalDir + QLatin1Char('/') + fileName;
    outcome.frames = info->frames;
    outcome.width = job.width();
    outcome.height = job.height();
    outcome.totalDelayMs = totalDelay;
    outcome.bytes = bytes;
    outcome.framesSha256 = QString::fromLatin1(frameHash.result().toHex());
    outcome.degraded = job.degraded();

    QString copiedAs; // the first copy's place in the folder, for the record of the audio
    if (keepCopy) {
        QDir().mkpath(partDir + QStringLiteral("/source"));
        int n = 0;
        for (const QString &src : job.sourcePaths()) {
            // numbered, so that two sources that sanitise to the same name do not collide
            const QString name = QStringLiteral("source/%1-%2").arg(++n, 3, 10, QLatin1Char('0')).arg(sanitiseOutputName(QFileInfo(src).fileName()));
            if (!copySource(src, partDir + QLatin1Char('/') + name, options.cancelled)) {
                cleanup();
                return fail(options.cancelled && options.cancelled() ? named(kCancelled) : named(kHelperFailed, QStringLiteral("the source cannot be copied")));
            }
            if (copiedAs.isEmpty())
                copiedAs = name;
        }
    }
    const QJsonValue sources = sourcesRecord(job.sourcePaths(), hashes);
    QJsonObject output{{QStringLiteral("file"), fileName},          {QStringLiteral("frames"), outcome.frames},
                       {QStringLiteral("width"), outcome.width},     {QStringLiteral("height"), outcome.height},
                       {QStringLiteral("totalDelayMs"), outcome.totalDelayMs}, {QStringLiteral("bytes"), outcome.bytes},
                       {QStringLiteral("framesSha256"), outcome.framesSha256}};
    const QJsonObject record{{QStringLiteral("version"), 1},
                             {QStringLiteral("tool"), QStringLiteral("murasaki-petimport 1")},
                             {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                             {QStringLiteral("spec"), QJsonDocument::fromJson(canonical).object()},
                             {QStringLiteral("sources"), sources},
                             {QStringLiteral("output"), output},
                             {QStringLiteral("degraded"), QJsonArray::fromStringList(outcome.degraded)}};
    // (the audio of a video, when the user asked to keep it, is inside the copy of the source in source/; phase 5 takes it from there)
    const QJsonObject recordWithAudio = spec.kind == ImportKind::Video && spec.keepAudio
        ? [&] { QJsonObject r = record; r.insert(QStringLiteral("audio"), QJsonObject{{QStringLiteral("hasAudio"), job.hasAudio()}, {QStringLiteral("kept"), !copiedAs.isEmpty()}, {QStringLiteral("file"), copiedAs}, {QStringLiteral("played"), false}}); return r; }()
        : record;
    QSaveFile recordFile(partDir + QStringLiteral("/source.json"));
    const QByteArray recordBytes = QJsonDocument(recordWithAudio).toJson(QJsonDocument::Indented);
    if (!recordFile.open(QIODevice::WriteOnly) || recordFile.write(recordBytes) != recordBytes.size() || !recordFile.commit()) {
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("source.json cannot be written")));
    }

    if (options.cancelled && options.cancelled()) {
        cleanup();
        return fail(named(kCancelled));
    }
    ImportOutcome kept;
    if (!publishFolder(partDir, finalDir, id, &kept)) {
        cleanup();
        return fail(named(kHelperFailed, QStringLiteral("the result cannot be moved into place")));
    }
    if (kept.ok)
        return kept;
    outcome.ok = true;
    return outcome;
}

} // namespace murasaki::import
