#pragma once

#include <QByteArray>
#include <QtEndian>
#include <QtGlobal>

#include <initializer_list>

// Synthetic animation files for the header reader tests. They carry valid structure (and CRCs, where the format
// has them) but meaningless pixels: the readers never decode. Never copy real Workshop content into tests.
namespace animfixtures {

inline quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffu;
    for (const char byte : data) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline QByteArray be32(quint32 value)
{
    QByteArray out(4, 0);
    qToBigEndian(value, out.data());
    return out;
}

inline QByteArray be16(quint16 value)
{
    QByteArray out(2, 0);
    qToBigEndian(value, out.data());
    return out;
}

inline QByteArray le16(quint16 value)
{
    QByteArray out(2, 0);
    qToLittleEndian(value, out.data());
    return out;
}

inline QByteArray le32(quint32 value)
{
    QByteArray out(4, 0);
    qToLittleEndian(value, out.data());
    return out;
}

inline QByteArray le24(quint32 value)
{
    return le32(value).left(3);
}

inline QByteArray pngChunk(const char *type, const QByteArray &data)
{
    const QByteArray typed = QByteArray(type, 4) + data;
    return be32(quint32(data.size())) + typed + be32(crc32(typed));
}

// A zlib stream holding `bytes` zero bytes (qCompress prefixes a 4-byte length that the PNG format does not have).
inline QByteArray zlibZeros(int bytes)
{
    return qCompress(QByteArray(bytes, 0), 9).mid(4);
}

struct Delay {
    quint16 num;
    quint16 den;
};

// An 8-bit RGBA APNG. `delays` has one entry per animation frame. With `defaultIsFrame` the first frame is the
// default image (its fcTL precedes IDAT), as encoders usually write it; otherwise IDAT is not part of the
// animation and every frame uses fdAT.
inline QByteArray apng(int width, int height, std::initializer_list<Delay> delays, bool defaultIsFrame = true)
{
    const int rowBytes = 1 + width * 4;
    const QByteArray pixels = zlibZeros(rowBytes * height);
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", be32(width) + be32(height) + QByteArray("\x08\x06\x00\x00\x00", 5));
    quint32 sequence = 0;
    if (delays.size() > 0)
        out += pngChunk("acTL", be32(quint32(delays.size())) + be32(0));
    const auto fcTL = [&](Delay delay) {
        return pngChunk("fcTL", be32(sequence++) + be32(width) + be32(height) + be32(0) + be32(0) + be16(delay.num)
                                    + be16(delay.den) + QByteArray("\x00\x00", 2));
    };
    bool first = true;
    for (const Delay &delay : delays) {
        if (first && defaultIsFrame) {
            out += fcTL(delay) + pngChunk("IDAT", pixels);
        } else {
            if (first)
                out += pngChunk("IDAT", pixels);
            out += fcTL(delay) + pngChunk("fdAT", be32(sequence++) + pixels);
        }
        first = false;
    }
    if (delays.size() == 0)
        out += pngChunk("IDAT", pixels);
    out += pngChunk("IEND", {});
    return out;
}

// GIF89a with a global color table. Delays are in centiseconds, one per frame; -1 omits the graphic control block.
inline QByteArray gif(int width, int height, std::initializer_list<int> delays)
{
    QByteArray out("GIF89a", 6);
    out += le16(width) + le16(height) + QByteArray("\x80\x00\x00", 3);
    out += QByteArray("\x00\x00\x00\xff\xff\xff", 6); // 2 colors
    out += QByteArray("\x21\xff\x0b" "NETSCAPE2.0" "\x03\x01\x00\x00\x00", 19); // loop extension with sub-blocks
    for (const int delay : delays) {
        if (delay >= 0)
            out += QByteArray("\x21\xf9\x04\x00", 4) + le16(quint16(delay)) + QByteArray("\x00\x00", 2);
        out += QByteArray("\x2c", 1) + le16(0) + le16(0) + le16(width) + le16(height) + QByteArray("\x00", 1);
        out += QByteArray("\x02\x02\x44\x01\x00", 5); // min code size 2, one 2-byte sub-block, terminator
    }
    out += QByteArray("\x3b", 1);
    return out;
}

// The same GIF with `frames` frames of one delay (centiseconds), for the files too big to list.
inline QByteArray gifFrames(int width, int height, int frames, int delay = 5)
{
    QByteArray out("GIF89a", 6);
    out += le16(width) + le16(height) + QByteArray("\x80\x00\x00", 3);
    out += QByteArray("\x00\x00\x00\xff\xff\xff", 6);
    out += QByteArray("\x21\xff\x0b" "NETSCAPE2.0" "\x03\x01\x00\x00\x00", 19);
    for (int i = 0; i < frames; ++i) {
        out += QByteArray("\x21\xf9\x04\x00", 4) + le16(quint16(delay)) + QByteArray("\x00\x00", 2);
        out += QByteArray("\x2c", 1) + le16(0) + le16(0) + le16(width) + le16(height) + QByteArray("\x00", 1);
        out += QByteArray("\x02\x02\x44\x01\x00", 5);
    }
    out += QByteArray("\x3b", 1);
    return out;
}

inline QByteArray riffChunk(const char *type, const QByteArray &data)
{
    QByteArray out = QByteArray(type, 4) + le32(quint32(data.size())) + data;
    if (data.size() & 1)
        out.append('\0');
    return out;
}

inline QByteArray riffWebp(const QByteArray &chunks)
{
    return QByteArray("RIFF", 4) + le32(quint32(4 + chunks.size())) + QByteArray("WEBP", 4) + chunks;
}

// Animated WebP: VP8X with the animation flag, ANIM, then one ANMF per duration (milliseconds).
inline QByteArray webpAnimated(int width, int height, std::initializer_list<int> durations)
{
    QByteArray chunks = riffChunk("VP8X", QByteArray("\x12\x00\x00\x00", 4) + le24(width - 1) + le24(height - 1));
    chunks += riffChunk("ANIM", le32(0) + le16(0));
    for (const int duration : durations) {
        const QByteArray frame = le24(0) + le24(0) + le24(width - 1) + le24(height - 1) + le24(duration)
            + QByteArray("\x00", 1) + riffChunk("VP8L", QByteArray("\x2f\x00\x00\x00\x00\x00\x00", 7));
        chunks += riffChunk("ANMF", frame);
    }
    return riffWebp(chunks);
}

// A lossless still image: VP8L, whose bitstream header holds the canvas size.
inline QByteArray webpStillLossless(int width, int height)
{
    const quint32 bits = quint32(width - 1) | (quint32(height - 1) << 14);
    return riffWebp(riffChunk("VP8L", QByteArray("\x2f", 1) + le32(bits) + QByteArray(8, 0)));
}

} // namespace animfixtures
