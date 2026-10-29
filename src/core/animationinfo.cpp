#include "animationinfo.h"

#include "petlimits.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int maxDimension = 32768;
constexpr int maxFrames = 1 << 20;
constexpr int maxLeadingPngChunks = 65; // chunks isAnimationFile walks: IHDR and 64 more; the acTL of a real APNG follows the IHDR
constexpr int maxGifRestarts = 64;       // GIF headers met after the first one (a file of joined GIFs)
constexpr qint64 maxGifSteps = 1 << 24; // sub-blocks walked per GIF (data comes in 255-byte pieces)
constexpr qint64 maxChunks = kMaxPetChunks; // chunks walked per APNG / WebP; real 300 MB APNGs use ~75k (4 KB fdAT pieces)

quint32 be32(const uchar *p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

quint32 le16(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8);
}

quint32 le24(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16);
}

quint32 le32(const uchar *p)
{
    return le24(p) | (quint32(p[3]) << 24);
}

bool validSize(qint64 width, qint64 height)
{
    return width >= 1 && height >= 1 && width <= maxDimension && height <= maxDimension;
}

// Reads exactly size bytes at the current position.
bool readExact(QIODevice *device, char *buffer, qint64 size)
{
    return device->read(buffer, size) == size;
}

double averageFps(int frames, double totalSeconds)
{
    if (frames < 2 || totalSeconds <= 0)
        return 0;
    return std::round(frames / totalSeconds * 100.0) / 100.0;
}

// Total size of the device, or -1 when it does not know it.
qint64 deviceSize(QIODevice *device)
{
    return device && device->isOpen() && device->isReadable() && !device->isSequential() ? device->size() : -1;
}

} // namespace

bool isAnimationExtension(const QString &suffix)
{
    const QString lower = suffix.toLower();
    return lower == u"apng" || lower == u"gif" || lower == u"webp";
}

bool isAnimationFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    if (isAnimationExtension(suffix))
        return true;
    if (suffix.compare(u"png", Qt::CaseInsensitive) != 0)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    static const char signature[8] = {char(0x89), 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    char head[8];
    if (!readExact(&file, head, 8) || memcmp(head, signature, 8) != 0)
        return false;
    const qint64 size = file.size();
    qint64 pos = 8;
    // Only the question "is this .png an animation": a small budget, since an item folder can hold hundreds of them, hostile ones
    // full of ancillary chunks included.
    for (qint64 step = 0; step < maxLeadingPngChunks; ++step) {
        uchar header[8];
        if (pos + 12 > size || !file.seek(pos) || !readExact(&file, reinterpret_cast<char *>(header), 8))
            return false;
        const quint32 length = be32(header);
        if (length > 0x7fffffffu || pos + 12 + qint64(length) > size)
            return false;
        const QByteArray type(reinterpret_cast<const char *>(header + 4), 4);
        if (type == "acTL")
            return length == 8;   // before any IDAT: the picture is an animation
        if (type == "IDAT" || type == "IEND")
            return false;         // the data came first (or there was none): a still picture
        pos += 12 + qint64(length);
    }
    return false;
}

std::optional<AnimationInfo> readApngInfo(QIODevice *device)
{
    const qint64 size = deviceSize(device);
    if (size < 8 + 25 || !device->seek(0))
        return std::nullopt;

    static const char signature[8] = {char(0x89), 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    char head[8];
    if (!readExact(device, head, 8) || memcmp(head, signature, 8) != 0)
        return std::nullopt;

    AnimationInfo info;
    info.format = AnimationFormat::Apng;
    bool haveHeader = false;
    bool haveEnd = false;
    bool sawData = false;
    bool haveControl = false;
    qint64 declaredFrames = -1;
    qint64 frames = 0;
    double totalSeconds = 0;

    qint64 pos = 8;
    for (qint64 step = 0; step < maxChunks; ++step) {
        uchar header[8];
        if (pos + 12 > size || !device->seek(pos) || !readExact(device, reinterpret_cast<char *>(header), 8))
            return std::nullopt;
        const quint32 length = be32(header);
        const QByteArray type(reinterpret_cast<const char *>(header + 4), 4);
        if (length > 0x7fffffffu || pos + 12 + qint64(length) > size)
            return std::nullopt;

        if (!haveHeader && type != "IHDR")
            return std::nullopt; // IHDR must come first
        if (type == "IHDR") {
            uchar data[13];
            if (haveHeader || length != 13 || !readExact(device, reinterpret_cast<char *>(data), 13))
                return std::nullopt;
            const quint32 width = be32(data);
            const quint32 height = be32(data + 4);
            if (!validSize(width, height))
                return std::nullopt;
            info.width = int(width);
            info.height = int(height);
            haveHeader = true;
        } else if (type == "acTL") {
            uchar data[8];
            if (sawData || declaredFrames >= 0 || length != 8 || !readExact(device, reinterpret_cast<char *>(data), 8))
                return std::nullopt;
            declaredFrames = be32(data);
            if (declaredFrames < 1 || declaredFrames > maxFrames)
                return std::nullopt;
        } else if (type == "fcTL") {
            uchar data[26];
            if (declaredFrames < 0 || length != 26 || !readExact(device, reinterpret_cast<char *>(data), 26))
                return std::nullopt;
            if (++frames > maxFrames)
                return std::nullopt;
            const quint32 num = (quint32(data[20]) << 8) | data[21];
            const quint32 den = (quint32(data[22]) << 8) | data[23];
            totalSeconds += petFrameDelayMs(1000.0 * double(num) / (den == 0 ? 100 : den)) / 1000.0;
            haveControl = true;
        } else if (type == "IDAT" || type == "fdAT") {
            sawData = true;
        } else if (type == "IEND") {
            haveEnd = true;
            break;
        }
        pos += 12 + qint64(length);
    }

    if (!haveHeader || !haveEnd || !sawData)
        return std::nullopt;
    if (declaredFrames < 0) { // a plain PNG: a still image
        info.frames = 1;
        return info;
    }
    if (!haveControl || frames != declaredFrames)
        return std::nullopt;
    info.frames = int(frames);
    info.fps = averageFps(info.frames, totalSeconds);
    return info;
}

std::optional<AnimationInfo> readGifInfo(QIODevice *device)
{
    const qint64 size = deviceSize(device);
    if (size < 13 + 1 || !device->seek(0))
        return std::nullopt;

    uchar head[13];
    if (!readExact(device, reinterpret_cast<char *>(head), 13)
        || (memcmp(head, "GIF87a", 6) != 0 && memcmp(head, "GIF89a", 6) != 0))
        return std::nullopt;

    AnimationInfo info;
    info.format = AnimationFormat::Gif;
    info.width = int(le16(head + 6));
    info.height = int(le16(head + 8));
    if (!validSize(info.width, info.height))
        return std::nullopt;

    qint64 pos = 13;
    if (head[10] & 0x80)
        pos += 3LL << ((head[10] & 7) + 1);

    qint64 frames = 0;
    qint64 pendingDelay = -1; // centiseconds from the last graphic control extension
    double totalSeconds = 0;
    bool haveTrailer = false;
    qint64 steps = 0;
    int restarts = 0;

    // Skips a chain of data sub-blocks starting at pos; returns false if truncated or too long.
    const auto skipSubBlocks = [&]() {
        while (steps++ < maxGifSteps) {
            uchar length;
            if (pos >= size || !device->seek(pos) || !readExact(device, reinterpret_cast<char *>(&length), 1))
                return false;
            pos += 1 + length;
            if (pos > size)
                return false;
            if (length == 0)
                return true;
        }
        return false;
    };

    while (steps++ < maxGifSteps) {
        uchar introducer;
        if (pos >= size || !device->seek(pos) || !readExact(device, reinterpret_cast<char *>(&introducer), 1))
            return std::nullopt;
        ++pos;
        if (introducer == 0x3b) {
            haveTrailer = true;
            break;
        }
        if (introducer == 0x21) {
            uchar ext[2];
            if (!readExact(device, reinterpret_cast<char *>(ext), 1))
                return std::nullopt;
            ++pos;
            if (ext[0] == 0xf9) { // graphic control: block size 4, flags, delay, transparent index
                uchar gce[5];
                if (!readExact(device, reinterpret_cast<char *>(gce), 5) || gce[0] != 4)
                    return std::nullopt;
                pendingDelay = le16(gce + 2);
                pos += 5;
            }
            if (!skipSubBlocks())
                return std::nullopt;
        } else if (introducer == 0x2c) {
            uchar desc[10]; // 9 bytes of descriptor plus the LZW minimum code size
            if (pos + 10 > size || !readExact(device, reinterpret_cast<char *>(desc), 9))
                return std::nullopt;
            pos += 9;
            if (desc[8] & 0x80)
                pos += 3LL << ((desc[8] & 7) + 1);
            ++pos; // LZW minimum code size
            if (!skipSubBlocks())
                return std::nullopt;
            if (++frames > maxFrames)
                return std::nullopt;
            totalSeconds += petFrameDelayMs(pendingDelay < 0 ? 0.0 : 10.0 * double(pendingDelay)) / 1000.0;
            pendingDelay = -1;
        } else if (introducer == 'G') {
            // another GIF appended to the first, without a trailer between (a file made by joining two): FFmpeg and Pillow carry on across the
            // second header, so its frames count. Anything else that is not a block ends the read, as before
            uchar rest[12]; // "IF89a" and the screen descriptor
            if (!readExact(device, reinterpret_cast<char *>(rest), 12) || memcmp(rest, "IF8", 3) != 0 || (rest[3] != '7' && rest[3] != '9') || rest[4] != 'a'
                || !validSize(le16(rest + 5), le16(rest + 7)) || ++restarts > maxGifRestarts)
                return std::nullopt;
            pos += 12;
            if (rest[9] & 0x80)
                pos += 3LL << ((rest[9] & 7) + 1);
            pendingDelay = -1;
        } else {
            return std::nullopt;
        }
    }

    if (!haveTrailer || frames < 1)
        return std::nullopt;
    info.frames = int(frames);
    info.fps = averageFps(info.frames, totalSeconds);
    return info;
}

std::optional<AnimationInfo> readWebpInfo(QIODevice *device)
{
    const qint64 size = deviceSize(device);
    if (size < 12 + 8 || !device->seek(0))
        return std::nullopt;

    uchar riff[12];
    if (!readExact(device, reinterpret_cast<char *>(riff), 12) || memcmp(riff, "RIFF", 4) != 0
        || memcmp(riff + 8, "WEBP", 4) != 0)
        return std::nullopt;
    // The RIFF size is advisory (Steam and encoders sometimes disagree); the file size is the hard bound.
    const qint64 end = qMin<qint64>(size, qint64(le32(riff + 4)) + 8);
    if (end < 12 + 8)
        return std::nullopt;

    AnimationInfo info;
    info.format = AnimationFormat::Webp;
    bool haveSize = false;
    bool animated = false;
    qint64 frames = 0;
    double totalSeconds = 0;
    bool sawStill = false;

    qint64 pos = 12;
    for (qint64 step = 0; step < maxChunks && pos + 8 <= end; ++step) {
        uchar header[8];
        if (!device->seek(pos) || !readExact(device, reinterpret_cast<char *>(header), 8))
            return std::nullopt;
        const quint32 length = le32(header + 4);
        const qint64 payload = pos + 8;
        if (payload + qint64(length) > end)
            return std::nullopt;
        const QByteArray type(reinterpret_cast<const char *>(header), 4);

        if (type == "VP8X") {
            uchar data[10];
            if (haveSize || length < 10 || !readExact(device, reinterpret_cast<char *>(data), 10))
                return std::nullopt;
            info.width = int(le24(data + 4)) + 1;
            info.height = int(le24(data + 7)) + 1;
            if (!validSize(info.width, info.height))
                return std::nullopt;
            animated = data[0] & 0x02;
            haveSize = true;
        } else if (type == "ANMF") {
            uchar data[16];
            if (!animated || length < 16 || !readExact(device, reinterpret_cast<char *>(data), 16))
                return std::nullopt;
            if (++frames > maxFrames)
                return std::nullopt;
            totalSeconds += petFrameDelayMs(double(le24(data + 12))) / 1000.0;
        } else if (!haveSize && type == "VP8L") {
            // A still image without VP8X: the canvas is in the bitstream header (signature byte + 4 bytes).
            uchar data[5];
            if (length < 5 || !readExact(device, reinterpret_cast<char *>(data), 5) || data[0] != 0x2f)
                return std::nullopt;
            const quint32 bits = le32(data + 1);
            info.width = int(bits & 0x3fff) + 1;
            info.height = int((bits >> 14) & 0x3fff) + 1;
            haveSize = true;
            sawStill = true;
        } else if (!haveSize && type == "VP8 ") {
            uchar data[10];
            if (length < 10 || !readExact(device, reinterpret_cast<char *>(data), 10) || data[3] != 0x9d
                || data[4] != 0x01 || data[5] != 0x2a)
                return std::nullopt;
            info.width = int(le16(data + 6) & 0x3fff);
            info.height = int(le16(data + 8) & 0x3fff);
            if (!validSize(info.width, info.height))
                return std::nullopt;
            haveSize = true;
            sawStill = true;
        } else if (type == "VP8L" || type == "VP8 ") {
            sawStill = true;
        }
        pos = payload + qint64(length) + (length & 1); // chunks are padded to even sizes
    }

    if (!haveSize)
        return std::nullopt;
    if (animated) {
        if (frames < 1)
            return std::nullopt;
        info.frames = int(frames);
        info.fps = averageFps(info.frames, totalSeconds);
        return info;
    }
    if (!sawStill)
        return std::nullopt;
    info.frames = 1;
    return info;
}

std::optional<AnimationInfo> readAnimationInfo(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray magic = file.read(12);
    if (magic.startsWith("\x89PNG"))
        return readApngInfo(&file);
    if (magic.startsWith("GIF8"))
        return readGifInfo(&file);
    if (magic.size() == 12 && magic.startsWith("RIFF") && magic.mid(8) == "WEBP")
        return readWebpInfo(&file);
    return std::nullopt;
}

} // namespace murasaki
