#include "boundedzip.h"

#include "petimport.h"

#include <QCryptographicHash>
#include <QIODevice>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>

#include <zlib.h>

#include <algorithm>
#include <climits>

namespace murasaki::import {
namespace {

constexpr quint32 kEndSignature = 0x06054b50;
constexpr quint32 kDirectorySignature = 0x02014b50;
constexpr quint32 kLocalSignature = 0x04034b50;
constexpr int kEndSize = 22;
constexpr int kDirectoryFixed = 46;
constexpr int kLocalFixed = 30;

QString named(std::string_view name, const QString &detail = {})
{
    QString out = QString::fromUtf8(name.data(), qsizetype(name.size()));
    if (!detail.isEmpty())
        out += QStringLiteral(": ") + detail;
    return out;
}

quint16 u16(const char *p) { return qFromLittleEndian<quint16>(p); }
quint32 u32(const char *p) { return qFromLittleEndian<quint32>(p); }

bool readAt(QIODevice *device, qint64 offset, char *buffer, qint64 length)
{
    return device->seek(offset) && device->read(buffer, length) == length;
}

} // namespace

QString normaliseZipName(const QByteArray &raw, bool utf8, int maxBytes, QString *why)
{
    const auto refuse = [why](const QString &message) {
        if (why)
            *why = message;
        return QString();
    };
    if (raw.isEmpty())
        return refuse(QStringLiteral("empty name"));
    if (raw.size() > maxBytes)
        return refuse(QStringLiteral("name over %1 bytes").arg(maxBytes));
    QString name;
    if (utf8) {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        name = decoder(raw);
        if (decoder.hasError())
            return refuse(QStringLiteral("name is not valid UTF-8"));
    } else {
        name = QString::fromLatin1(raw); // CP437 for people; the bytes only matter for the checks below
    }
    for (const QChar c : name) {
        // Without the UTF-8 flag the bytes are CP437 (e-acute is 0x82): only the C0 controls and DEL are refused, nothing is ever written under the name.
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || (utf8 && c.unicode() >= 0x80 && c.unicode() < 0xa0))
            return refuse(QStringLiteral("control character in the name"));
        if (c == QChar(0x202e) || c == QChar(0x2066) || c == QChar(0x2067) || c == QChar(0x2068) || c == QChar(0x2069) || c == QChar(0x200e)
            || c == QChar(0x200f) || c == QChar(0x202a) || c == QChar(0x202b) || c == QChar(0x202c) || c == QChar(0x202d))
            return refuse(QStringLiteral("bidirectional control in the name"));
    }
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (name.startsWith(QLatin1Char('/')))
        return refuse(QStringLiteral("absolute path"));
    if (name.size() >= 2 && name.at(1) == QLatin1Char(':') && name.at(0).isLetter())
        return refuse(QStringLiteral("drive letter"));
    const QStringList parts = name.split(QLatin1Char('/'));
    for (int i = 0; i < parts.size(); ++i) {
        if (parts.at(i) == QLatin1String("..") || parts.at(i) == QLatin1String("."))
            return refuse(QStringLiteral("a '%1' component").arg(parts.at(i)));
        if (parts.at(i).isEmpty() && i + 1 != parts.size())
            return refuse(QStringLiteral("an empty component"));
    }
    return name;
}

std::optional<BoundedZip> BoundedZip::open(QIODevice *device, const ZipLimits &limits, QString *error)
{
    const auto fail = [error](std::string_view name, const QString &detail = {}) -> std::optional<BoundedZip> {
        if (error)
            *error = named(name, detail);
        return std::nullopt;
    };
    if (!device || !device->isReadable() || device->isSequential())
        return fail(kZipNotAZip, QStringLiteral("unreadable"));
    const qint64 size = device->size();
    if (size < kEndSize)
        return fail(kZipNotAZip, QStringLiteral("too short"));

    // The end record sits exactly at the end, less its comment. Scan back over at most 64 KiB of comment, and demand that the comment length fits.
    const qint64 window = std::min<qint64>(size, kEndSize + 65535);
    QByteArray tail(int(window), 0);
    if (!readAt(device, size - window, tail.data(), window))
        return fail(kZipNotAZip, QStringLiteral("cannot read the end"));
    qint64 end = -1;
    for (qint64 i = window - kEndSize; i >= 0; --i) {
        if (u32(tail.constData() + i) == kEndSignature && i + kEndSize + u16(tail.constData() + i + 20) == window) {
            end = i;
            break;
        }
    }
    if (end < 0)
        return fail(kZipNotAZip, QStringLiteral("no end record"));
    const char *record = tail.constData() + end;
    const quint16 disk = u16(record + 4), directoryDisk = u16(record + 6), onDisk = u16(record + 8), total = u16(record + 10);
    const quint32 directorySize = u32(record + 12), directoryOffset = u32(record + 16);
    if (disk != 0 || directoryDisk != 0 || onDisk != total)
        return fail(kZipNotAZip, QStringLiteral("several disks"));
    if (total == 0xFFFF || directorySize == 0xFFFFFFFFu || directoryOffset == 0xFFFFFFFFu)
        return fail(kZipNotAZip, QStringLiteral("ZIP64 is not supported"));
    if (total > limits.maxEntries)
        return fail(kZipOverTheLimit, QStringLiteral("%1 entries").arg(total));
    const qint64 endOffset = size - window + end;
    if (qint64(directoryOffset) + directorySize != endOffset)
        return fail(kZipNotAZip, QStringLiteral("the central directory does not end where the end record starts"));
    if (directorySize < qint64(total) * kDirectoryFixed)
        return fail(kZipNotAZip, QStringLiteral("the central directory is too small for its entries"));
    // Nothing is reserved before this: a record is at most 46 bytes, a name, an extra field and a comment, and the whole is capped as well.
    const qint64 mostPerRecord = qint64(kDirectoryFixed) + limits.maxNameBytes + 2LL * 65535;
    if (qint64(directorySize) > qint64(total) * mostPerRecord || qint64(directorySize) > limits.maxDirectoryBytes || qint64(directorySize) > INT_MAX)
        return fail(kZipOverTheLimit, QStringLiteral("the central directory is %1 bytes").arg(directorySize));

    QByteArray directory(static_cast<qsizetype>(directorySize), 0);
    if (!readAt(device, directoryOffset, directory.data(), qint64(directorySize)))
        return fail(kZipNotAZip, QStringLiteral("cannot read the central directory"));

    BoundedZip zip;
    zip.m_device = device;
    zip.m_limits = limits;
    zip.m_directoryOffset = directoryOffset;
    QSet<QString> seen;
    long long declaredTotal = 0;
    qint64 pos = 0;
    struct Range {
        qint64 begin;
        qint64 end;
    };
    QList<Range> ranges;
    for (int i = 0; i < total; ++i) {
        if (pos + kDirectoryFixed > directory.size() || u32(directory.constData() + pos) != kDirectorySignature)
            return fail(kZipNotAZip, QStringLiteral("entry %1 is not a directory record").arg(i));
        const char *p = directory.constData() + pos;
        const quint16 madeBy = u16(p + 4), flags = u16(p + 8), method = u16(p + 10);
        const quint32 crc = u32(p + 16), compressed = u32(p + 20), uncompressed = u32(p + 24);
        const quint16 nameLength = u16(p + 28), extraLength = u16(p + 30), commentLength = u16(p + 32);
        const quint32 externalAttributes = u32(p + 38), localOffset = u32(p + 42);
        const qint64 next = pos + kDirectoryFixed + nameLength + extraLength + commentLength;
        if (next > directory.size())
            return fail(kZipNotAZip, QStringLiteral("entry %1 runs past the directory").arg(i));
        const QByteArray rawName = directory.mid(int(pos + kDirectoryFixed), nameLength);
        pos = next;
        QString why;
        const QString name = normaliseZipName(rawName, flags & 0x0800, limits.maxNameBytes, &why);
        if (name.isEmpty())
            return fail(kZipEntryRefused, QStringLiteral("entry %1: %2").arg(i).arg(why));
        const bool directoryEntry = name.endsWith(QLatin1Char('/'));
        const QString shown = name.left(120);
        if (flags & 0x0001)
            return fail(kZipEntryRefused, QStringLiteral("%1: encrypted").arg(shown));
        if (flags & 0x0040)
            return fail(kZipEntryRefused, QStringLiteral("%1: strong encryption").arg(shown));
        if ((madeBy >> 8) == 3 && ((externalAttributes >> 16) & 0170000) == 0120000)
            return fail(kZipEntryRefused, QStringLiteral("%1: symbolic link").arg(shown));
        if (!directoryEntry && method != 0 && method != 8)
            return fail(kZipEntryRefused, QStringLiteral("%1: method %2").arg(shown).arg(method));
        if (compressed == 0xFFFFFFFFu || uncompressed == 0xFFFFFFFFu || localOffset == 0xFFFFFFFFu)
            return fail(kZipNotAZip, QStringLiteral("ZIP64 is not supported"));
        const QString key = (directoryEntry ? name.chopped(1) : name).toCaseFolded();
        if (seen.contains(key))
            return fail(kZipEntryRefused, QStringLiteral("%1: duplicate name").arg(shown));
        seen.insert(key);
        ZipEntry entry;
        entry.name = name;
        entry.isDirectory = directoryEntry;
        entry.method = method;
        entry.compressedSize = compressed;
        entry.size = uncompressed;
        entry.crc = crc;
        entry.localOffset = localOffset;
        if (!directoryEntry) {
            bool tooBig = false;
            if (uncompressed > limits.maxMemberBytes)
                tooBig = true;
            else if (uncompressed >= 4096 && static_cast<long long>(uncompressed) > static_cast<long long>(compressed) * limits.maxRatio)
                tooBig = true;
            if (tooBig && !limits.skipOversize) {
                if (uncompressed > limits.maxMemberBytes)
                    return fail(kZipOverTheLimit, QStringLiteral("%1: %2 bytes").arg(shown).arg(uncompressed));
                return fail(kZipOverTheLimit, QStringLiteral("%1: compression ratio over %2:1").arg(shown).arg(limits.maxRatio));
            }
            entry.tooBig = tooBig;
            if (!tooBig) {
                declaredTotal += uncompressed;
                if (declaredTotal > limits.maxTotalBytes)
                    return fail(kZipOverTheLimit, QStringLiteral("more than %1 bytes in all").arg(limits.maxTotalBytes));
            }
            if (method == 0 && compressed != uncompressed)
                return fail(kZipEntryRefused, QStringLiteral("%1: stored with two sizes").arg(shown));
            // The local header must agree with this record, and the bytes start where IT says: that range is what is checked against the directory and the
            // other members (a long local extra field cannot move the data under another member).
            char header[kLocalFixed];
            if (qint64(localOffset) + kLocalFixed > qint64(directoryOffset) || !readAt(device, localOffset, header, kLocalFixed) || u32(header) != kLocalSignature)
                return fail(kZipEntryRefused, QStringLiteral("%1: bad local header").arg(shown));
            const quint16 localFlags = u16(header + 6), localMethod = u16(header + 8), localName = u16(header + 26), localExtra = u16(header + 28);
            if (localMethod != method || (localFlags & ~0x0008) != (flags & ~0x0008) || localName != nameLength)
                return fail(kZipEntryRefused, QStringLiteral("%1: the local header disagrees with the directory").arg(shown));
            QByteArray localNameBytes(nameLength, 0);
            if (!readAt(device, qint64(localOffset) + kLocalFixed, localNameBytes.data(), nameLength) || localNameBytes != rawName)
                return fail(kZipEntryRefused, QStringLiteral("%1: the local name differs from the directory's").arg(shown));
            const qint64 dataOffset = qint64(localOffset) + kLocalFixed + localName + localExtra;
            const qint64 stop = dataOffset + qint64(compressed);
            if (stop > qint64(directoryOffset))
                return fail(kZipEntryRefused, QStringLiteral("%1: data outside the file").arg(shown));
            entry.dataOffset = dataOffset;
            ranges.append({qint64(localOffset), stop});
        }
        zip.m_entries.append(entry);
    }
    if (pos != directory.size())
        return fail(kZipNotAZip, QStringLiteral("bytes after the last directory record"));
    std::sort(ranges.begin(), ranges.end(), [](const Range &a, const Range &b) { return a.begin < b.begin; });
    for (qsizetype i = 1; i < ranges.size(); ++i) {
        if (ranges.at(i).begin < ranges.at(i - 1).end)
            return fail(kZipEntryRefused, QStringLiteral("members overlap"));
    }
    return zip;
}

bool BoundedZip::stream(int index, const std::function<bool(const char *, qsizetype)> &sink, QString *error) const
{
    QString why;
    const bool ok = [&] {
        if (index < 0 || index >= m_entries.size() || m_entries.at(index).isDirectory) {
            why = QStringLiteral("no such member");
            return false;
        }
        const ZipEntry &entry = m_entries.at(index);
        const QString shown = entry.name.left(120);
        if (!m_device || !m_device->seek(entry.localOffset)) {
            why = QStringLiteral("cannot seek");
            return false;
        }
        char header[kLocalFixed];
        if (m_device->read(header, kLocalFixed) != kLocalFixed || u32(header) != kLocalSignature) {
            why = shown + QStringLiteral(": bad local header");
            return false;
        }
        if (entry.tooBig) {
            why = shown + QStringLiteral(": over the member limit, skipped");
            return false;
        }
        const qint64 dataOffset = entry.dataOffset;
        if (!m_device->seek(dataOffset)) {
            why = shown + QStringLiteral(": cannot seek to the data");
            return false;
        }
        z_stream zs{};
        const bool deflated = entry.method == 8;
        if (deflated && inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            why = QStringLiteral("zlib cannot start");
            return false;
        }
        QByteArray in(64 * 1024, 0), out(64 * 1024, 0);
        quint64 remaining = entry.compressedSize, total = 0;
        uLong crc = crc32(0L, Z_NULL, 0);
        bool good = true, ended = !deflated;
        const auto deliver = [&](const char *data, qsizetype length) {
            total += quint64(length);
            if (total > entry.size) { // more than the directory declared: a bomb, or a lie
                why = shown + QStringLiteral(": larger than declared");
                return false;
            }
            crc = crc32(crc, reinterpret_cast<const Bytef *>(data), uInt(length));
            if (!sink(data, length)) {
                why = QStringLiteral("the output refused the data");
                return false;
            }
            return true;
        };
        if (!deflated) {
            while (good && remaining > 0) {
                const qint64 take = qint64(std::min<quint64>(remaining, quint64(in.size())));
                if (m_device->read(in.data(), take) != take) {
                    why = shown + QStringLiteral(": truncated");
                    good = false;
                    break;
                }
                remaining -= quint64(take);
                good = deliver(in.constData(), qsizetype(take));
            }
        } else {
            while (good && !ended) {
                if (zs.avail_in == 0 && remaining > 0) {
                    const qint64 take = qint64(std::min<quint64>(remaining, quint64(in.size())));
                    if (m_device->read(in.data(), take) != take) {
                        why = shown + QStringLiteral(": truncated");
                        good = false;
                        break;
                    }
                    remaining -= quint64(take);
                    zs.next_in = reinterpret_cast<Bytef *>(in.data());
                    zs.avail_in = uInt(take);
                }
                zs.next_out = reinterpret_cast<Bytef *>(out.data());
                zs.avail_out = uInt(out.size());
                const int status = inflate(&zs, Z_NO_FLUSH);
                const qsizetype produced = out.size() - qsizetype(zs.avail_out);
                if (produced > 0 && !deliver(out.constData(), produced)) {
                    good = false;
                    break;
                }
                if (status == Z_STREAM_END) {
                    ended = true;
                } else if (status != Z_OK && status != Z_BUF_ERROR) {
                    why = shown + QStringLiteral(": damaged data");
                    good = false;
                } else if (produced == 0 && zs.avail_in == 0 && remaining == 0) {
                    why = shown + QStringLiteral(": truncated stream");
                    good = false;
                }
            }
            if (good && (zs.avail_in != 0 || remaining != 0)) {
                why = shown + QStringLiteral(": data after the end of the stream");
                good = false;
            }
            inflateEnd(&zs);
        }
        if (good && total != entry.size) {
            why = shown + QStringLiteral(": size differs from the directory");
            good = false;
        }
        if (good && quint32(crc) != entry.crc) {
            why = shown + QStringLiteral(": bad CRC");
            good = false;
        }
        return good;
    }();
    if (!ok && error)
        *error = named(kZipEntryRefused, why);
    return ok;
}

std::optional<QByteArray> BoundedZip::read(int index, QString *error) const
{
    if (index >= 0 && index < m_entries.size() && m_entries.at(index).size > m_limits.maxMemoryBytes) {
        if (error)
            *error = named(kZipOverTheLimit, m_entries.at(index).name.left(120) + QStringLiteral(": too big to read into memory"));
        return std::nullopt;
    }
    QByteArray out;
    if (!stream(index, [&out](const char *data, qsizetype length) { out.append(data, length); return true; }, error))
        return std::nullopt;
    return out;
}

std::optional<QByteArray> BoundedZip::head(int index, qsizetype bytes, QString *error) const
{
    QByteArray out;
    bool enough = false;
    QString why;
    const bool ok = stream(
        index,
        [&](const char *data, qsizetype length) {
            out.append(data, qMin(length, bytes - out.size()));
            enough = out.size() >= bytes;
            return !enough;
        },
        &why);
    if (!ok && !enough) {
        if (error)
            *error = why;
        return std::nullopt;
    }
    return out;
}

bool BoundedZip::extractTo(int index, QIODevice *sink, QString *error) const
{
    return stream(index, [sink](const char *data, qsizetype length) { return sink->write(data, length) == length; }, error);
}

std::optional<QByteArray> BoundedZip::sha256(int index, QString *error) const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!stream(index, [&hash](const char *data, qsizetype length) { hash.addData(QByteArrayView(data, length)); return true; }, error))
        return std::nullopt;
    return hash.result();
}

} // namespace murasaki::import
