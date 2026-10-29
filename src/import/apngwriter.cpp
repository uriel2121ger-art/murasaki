#include "apngwriter.h"

#include "petimport.h"

#include <QIODevice>
#include <QtEndian>

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace murasaki::import {
namespace {

constexpr qsizetype kChunkBytes = 256 * 1024;

QByteArray be32(quint32 value)
{
    QByteArray out(4, 0);
    qToBigEndian(value, out.data());
    return out;
}

QByteArray be16(quint16 value)
{
    QByteArray out(2, 0);
    qToBigEndian(value, out.data());
    return out;
}

quint32 crcOf(const char *type, const QByteArray &data)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(type), 4);
    if (!data.isEmpty())
        crc = crc32(crc, reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size()));
    return quint32(crc);
}

// The sum of the absolute values of the filtered bytes read as signed: the usual heuristic for the best filter of a row.
long long cost(const uchar *row, qsizetype n)
{
    long long sum = 0;
    for (qsizetype i = 0; i < n; ++i)
        sum += std::abs(int(qint8(row[i])));
    return sum;
}

} // namespace

ApngWriter::ApngWriter(QIODevice *device, long long maxBytes)
    : m_device(device)
    , m_maxBytes(maxBytes)
{
}

bool ApngWriter::fail(const QString &message)
{
    m_dead = true;
    if (m_error.isEmpty())
        m_error = message;
    return false;
}

bool ApngWriter::write(const QByteArray &bytes)
{
    if (m_written + bytes.size() > m_maxBytes) {
        m_overBudget = true;
        return fail(QStringLiteral("output over the budget"));
    }
    if (m_device->write(bytes) != bytes.size())
        return fail(QStringLiteral("cannot write the output: ") + m_device->errorString());
    m_written += bytes.size();
    return true;
}

bool ApngWriter::writeChunk(const char *type, const QByteArray &data)
{
    return write(be32(quint32(data.size())) + QByteArray(type, 4) + data + be32(crcOf(type, data)));
}

bool ApngWriter::begin(int width, int height)
{
    if (m_begun || m_dead)
        return fail(QStringLiteral("begin() called twice"));
    if (width < 1 || height < 1 || width > kMaxCanvasSide || height > kMaxCanvasSide)
        return fail(QStringLiteral("canvas outside 1 to %1 pixels a side").arg(kMaxCanvasSide));
    if (!m_device || !m_device->isWritable() || m_device->isSequential())
        return fail(QStringLiteral("the output must be a seekable device open for writing"));
    m_width = width;
    m_height = height;
    m_begun = true;
    if (!write(QByteArray("\x89PNG\r\n\x1a\n", 8)))
        return false;
    if (!writeChunk("IHDR", be32(quint32(width)) + be32(quint32(height)) + QByteArray("\x08\x06\x00\x00\x00", 5)))
        return false;
    m_actlOffset = m_device->pos() + 8; // after the length and the type
    return writeChunk("acTL", be32(0) + be32(0));
}

bool ApngWriter::addFrame(const uchar *rgba, qsizetype stride, int delayMs)
{
    if (!m_begun || m_dead)
        return fail(QStringLiteral("addFrame() on a writer that is not open"));
    if (m_frames >= kMaxFrames)
        return fail(QStringLiteral("more than %1 frames").arg(kMaxFrames));
    if (!rgba || stride < qsizetype(m_width) * 4)
        return fail(QStringLiteral("bad frame buffer"));
    delayMs = std::clamp(delayMs, 0, 65535 * 1000);
    // delay = num / den seconds; keep the millisecond while it fits 16 bits, else the centisecond
    quint16 num = quint16(delayMs), den = 1000;
    if (delayMs > 65535) {
        num = quint16(std::min(65535, (delayMs + 5) / 10));
        den = 100;
    }
    const QByteArray control = be32(m_sequence++) + be32(quint32(m_width)) + be32(quint32(m_height)) + be32(0) + be32(0) + be16(num) + be16(den)
        + QByteArray("\x00\x00", 2); // dispose none, blend source
    if (!writeChunk("fcTL", control))
        return false;

    // filter the rows (one at a time into the deflate input) and stream the compressed bytes out as chunks
    z_stream zs{};
    if (deflateInit(&zs, 6) != Z_OK)
        return fail(QStringLiteral("zlib cannot start"));
    const bool first = m_frames == 0;
    const qsizetype rowBytes = qsizetype(m_width) * 4;
    QByteArray filtered(1 + rowBytes, 0);
    QByteArray none(rowBytes, 0), sub(rowBytes, 0), up(rowBytes, 0);
    QByteArray pending;
    pending.reserve(kChunkBytes + 1024);
    QByteArray out(64 * 1024, 0);
    bool ok = true;
    const auto flush = [&](bool force) {
        while (ok && (pending.size() >= kChunkBytes || (force && !pending.isEmpty()))) {
            const qsizetype take = std::min<qsizetype>(pending.size(), kChunkBytes);
            const QByteArray piece = pending.left(take);
            pending.remove(0, take);
            ok = first ? writeChunk("IDAT", piece) : writeChunk("fdAT", be32(m_sequence++) + piece);
        }
    };
    const auto pump = [&](int mode) {
        for (;;) {
            zs.next_out = reinterpret_cast<Bytef *>(out.data());
            zs.avail_out = uInt(out.size());
            const int status = deflate(&zs, mode);
            if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
                ok = fail(QStringLiteral("zlib failed"));
                return;
            }
            pending.append(out.constData(), out.size() - qsizetype(zs.avail_out));
            flush(false);
            if (!ok || zs.avail_out != 0 || status == Z_STREAM_END)
                return;
        }
    };
    for (int y = 0; y < m_height && ok; ++y) {
        const uchar *row = rgba + qsizetype(y) * stride;
        const uchar *above = y > 0 ? row - stride : nullptr;
        uchar *n = reinterpret_cast<uchar *>(none.data());
        uchar *s = reinterpret_cast<uchar *>(sub.data());
        uchar *u = reinterpret_cast<uchar *>(up.data());
        std::memcpy(n, row, size_t(rowBytes));
        for (qsizetype i = 0; i < rowBytes; ++i) {
            s[i] = uchar(row[i] - (i >= 4 ? row[i - 4] : 0));
            u[i] = uchar(row[i] - (above ? above[i] : 0));
        }
        const long long cn = cost(n, rowBytes), cs = cost(s, rowBytes), cu = above ? cost(u, rowBytes) : cn + 1;
        uchar type = 0;
        const uchar *best = n;
        long long bestCost = cn;
        if (cs < bestCost) {
            type = 1;
            best = s;
            bestCost = cs;
        }
        if (cu < bestCost) {
            type = 2;
            best = u;
        }
        filtered[0] = char(type);
        std::memcpy(filtered.data() + 1, best, size_t(rowBytes));
        zs.next_in = reinterpret_cast<Bytef *>(filtered.data());
        zs.avail_in = uInt(filtered.size());
        while (ok && zs.avail_in > 0)
            pump(Z_NO_FLUSH);
    }
    if (ok) {
        zs.avail_in = 0;
        zs.next_in = nullptr;
        pump(Z_FINISH);
        flush(true);
    }
    deflateEnd(&zs);
    if (!ok)
        return false;
    ++m_frames;
    return true;
}

bool ApngWriter::finish()
{
    if (!m_begun || m_dead)
        return fail(QStringLiteral("finish() on a writer that is not open"));
    if (m_frames < 1)
        return fail(QStringLiteral("an animation needs at least one frame"));
    if (!writeChunk("IEND", {}))
        return false;
    // patch the frame count of acTL (and its CRC)
    const QByteArray data = be32(quint32(m_frames)) + be32(0);
    const qint64 end = m_device->pos();
    if (!m_device->seek(m_actlOffset) || m_device->write(data) != data.size() || m_device->write(be32(crcOf("acTL", data))) != 4
        || !m_device->seek(end))
        return fail(QStringLiteral("cannot patch the frame count"));
    m_dead = true; // finished
    return true;
}

} // namespace murasaki::import
