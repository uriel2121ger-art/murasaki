#pragma once

// Fixtures of the import tests: an independent APNG decoder (every PNG filter, written from the specification and not sharing a line with the writer) and
// a ZIP builder that can also write the malformed archives the reader must refuse.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtEndian>

#include <zlib.h>

#include <cstdlib>
#include <cstring>

namespace importfixtures {

struct DecodedApng {
    bool ok = false;
    QString problem;
    int width = 0;
    int height = 0;
    int declaredFrames = -1;
    QList<QByteArray> frames; // width * height * 4 bytes each, straight RGBA
    QList<double> delaysMs;
};

inline quint32 be32(const char *p) { return qFromBigEndian<quint32>(p); }

inline QByteArray inflateZlib(const QByteArray &data, qsizetype expected)
{
    QByteArray out(int(expected), 0);
    uLongf size = uLongf(expected);
    if (uncompress(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(data.constData()), uLong(data.size())) != Z_OK
        || qsizetype(size) != expected)
        return {};
    return out;
}

inline QByteArray unfilter(const QByteArray &raw, int width, int height)
{
    const int bpp = 4;
    const int row = width * bpp;
    if (raw.size() != qsizetype(height) * (row + 1))
        return {};
    QByteArray out(height * row, 0);
    for (int y = 0; y < height; ++y) {
        const uchar type = uchar(raw[y * (row + 1)]);
        const uchar *src = reinterpret_cast<const uchar *>(raw.constData()) + y * (row + 1) + 1;
        uchar *dst = reinterpret_cast<uchar *>(out.data()) + y * row;
        const uchar *prev = y ? dst - row : nullptr;
        for (int i = 0; i < row; ++i) {
            const int a = i >= bpp ? dst[i - bpp] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int predictor = 0;
            switch (type) {
            case 0: predictor = 0; break;
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return {};
            }
            dst[i] = uchar(src[i] + predictor);
        }
    }
    return out;
}

// Reads an APNG whose frames are all full-canvas, blend source, dispose none (what ApngWriter writes), checking the chunk order, the sequence numbers
// and every CRC on the way.
inline DecodedApng decodeApng(const QByteArray &file)
{
    DecodedApng result;
    const auto bad = [&result](const QString &why) {
        result.problem = why;
        return result;
    };
    if (!file.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)))
        return bad(QStringLiteral("signature"));
    qsizetype pos = 8;
    quint32 sequence = 0;
    QByteArray current;       // compressed data of the frame being read
    bool haveFrame = false, sawEnd = false, sawHeader = false;
    double delay = 0;
    const auto closeFrame = [&]() -> bool {
        if (!haveFrame)
            return true;
        const QByteArray raw = inflateZlib(current, qsizetype(result.height) * (1 + result.width * 4));
        const QByteArray pixels = unfilter(raw, result.width, result.height);
        if (pixels.isEmpty())
            return false;
        result.frames.append(pixels);
        result.delaysMs.append(delay);
        current.clear();
        haveFrame = false;
        return true;
    };
    while (pos + 12 <= file.size() && !sawEnd) {
        const quint32 length = be32(file.constData() + pos);
        if (length > 0x7fffffffu || pos + 12 + qsizetype(length) > file.size())
            return bad(QStringLiteral("chunk past the end"));
        const QByteArray type = file.mid(pos + 4, 4);
        const QByteArray data = file.mid(pos + 8, int(length));
        const uLong crc = crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef *>(file.constData() + pos + 4), uInt(4 + length));
        if (quint32(crc) != be32(file.constData() + pos + 8 + length))
            return bad(QStringLiteral("bad CRC in ") + QString::fromLatin1(type));
        if (type == "IHDR") {
            if (length != 13 || data[8] != 8 || data[9] != 6 || data[10] != 0 || data[11] != 0 || data[12] != 0)
                return bad(QStringLiteral("IHDR is not 8-bit RGBA"));
            result.width = int(be32(data.constData()));
            result.height = int(be32(data.constData() + 4));
            sawHeader = true;
        } else if (type == "acTL") {
            if (!sawHeader || length != 8)
                return bad(QStringLiteral("acTL"));
            result.declaredFrames = int(be32(data.constData()));
        } else if (type == "fcTL") {
            if (!closeFrame())
                return bad(QStringLiteral("frame data"));
            if (length != 26 || be32(data.constData()) != sequence++)
                return bad(QStringLiteral("fcTL sequence"));
            if (int(be32(data.constData() + 4)) != result.width || int(be32(data.constData() + 8)) != result.height || be32(data.constData() + 12) != 0
                || be32(data.constData() + 16) != 0 || data[24] != 0 || data[25] != 0)
                return bad(QStringLiteral("fcTL is not full canvas / dispose none / blend source"));
            const quint16 num = qFromBigEndian<quint16>(data.constData() + 20), den = qFromBigEndian<quint16>(data.constData() + 22);
            delay = 1000.0 * num / (den ? den : 100);
            haveFrame = true;
        } else if (type == "IDAT") {
            if (!haveFrame || result.frames.size() != 0)
                return bad(QStringLiteral("IDAT outside the first frame"));
            current.append(data);
        } else if (type == "fdAT") {
            if (!haveFrame || length < 4 || be32(data.constData()) != sequence++)
                return bad(QStringLiteral("fdAT sequence"));
            current.append(data.mid(4));
        } else if (type == "IEND") {
            if (!closeFrame())
                return bad(QStringLiteral("frame data"));
            sawEnd = true;
        }
        pos += 12 + qsizetype(length);
    }
    if (!sawEnd || pos != file.size())
        return bad(QStringLiteral("no IEND at the end"));
    result.ok = true;
    return result;
}

// ---- ZIP ----

struct ZipMember {
    QByteArray name;
    QByteArray data;
    bool deflate = true;
    quint16 flags = 0;
    quint16 madeBy = 0x031e;       // Unix, version 3.0
    quint32 externalAttributes = 0100644u << 16;
    quint16 method = 0xFFFF;       // 0xFFFF: from `deflate`
    // lies for the hostile archives
    bool lieAboutSize = false;
    quint32 declaredSize = 0;
    bool badCrc = false;
    bool noLocalHeader = false;
    bool hasLocalName = false;     // the local header carries `localName` instead of `name`
    QByteArray localName;
    QByteArray localExtra;         // an extra field in the local header only (moves the data)
};

inline QByteArray rawDeflate(const QByteArray &data)
{
    z_stream zs{};
    deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&zs, uLong(data.size()))) + 64, 0);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    zs.avail_in = uInt(data.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(int(zs.total_out));
    deflateEnd(&zs);
    return out;
}

inline QByteArray le16(quint16 v)
{
    QByteArray b(2, 0);
    qToLittleEndian(v, b.data());
    return b;
}

inline QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}

// A well-formed ZIP of the members, or with the members' lies; `comment` is placed after the end record, `directoryGap` bytes are inserted between the
// last member and the central directory.
inline QByteArray buildZip(const QList<ZipMember> &members, const QByteArray &comment = {}, int totalOverride = -1)
{
    QByteArray file, directory;
    for (const ZipMember &m : members) {
        const quint16 method = m.method != 0xFFFF ? m.method : (m.deflate ? 8 : 0);
        const QByteArray payload = method == 8 ? rawDeflate(m.data) : m.data;
        const quint32 crc = m.badCrc ? 0x12345678u
                                     : quint32(crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef *>(m.data.constData()), uInt(m.data.size())));
        const quint32 size = m.lieAboutSize ? m.declaredSize : quint32(m.data.size());
        const quint32 offset = quint32(file.size());
        if (!m.noLocalHeader) {
            const QByteArray lname = m.hasLocalName ? m.localName : m.name;
            file += le32(0x04034b50) + le16(20) + le16(m.flags) + le16(method) + le16(0) + le16(0x21) + le32(crc) + le32(quint32(payload.size())) + le32(size)
                + le16(quint16(lname.size())) + le16(quint16(m.localExtra.size())) + lname + m.localExtra + payload;
        } else {
            file += QByteArray(30 + m.name.size(), 'x') + payload;
        }
        directory += le32(0x02014b50) + le16(m.madeBy) + le16(20) + le16(m.flags) + le16(method) + le16(0) + le16(0x21) + le32(crc) + le32(quint32(payload.size()))
            + le32(size) + le16(quint16(m.name.size())) + le16(0) + le16(0) + le16(0) + le16(0) + le32(m.externalAttributes) + le32(offset) + m.name;
    }
    const quint32 directoryOffset = quint32(file.size());
    const quint16 count = quint16(totalOverride >= 0 ? totalOverride : members.size());
    return file + directory + le32(0x06054b50) + le16(0) + le16(0) + le16(count) + le16(count) + le32(quint32(directory.size())) + le32(directoryOffset)
        + le16(quint16(comment.size())) + comment;
}

} // namespace importfixtures
