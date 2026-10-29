#pragma once

#include <QByteArray>
#include <QSize>
#include <QtEndian>

namespace murasaki::import {

// The image format of a file by its first bytes, for the formats the importer reads and no others: "png", "jpeg", "webp", "gif". nullptr for anything
// else. The importer gives this name to QImageReader as the format, so no other plugin (the system has many) ever parses a hostile file, and a file's
// extension decides nothing.
inline const char *sniffImageFormat(const QByteArray &head)
{
    if (head.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)))
        return "png";
    if (head.size() >= 3 && head.startsWith(QByteArray("\xff\xd8\xff", 3)))
        return "jpeg";
    if (head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP")
        return "webp";
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
        return "gif";
    return nullptr;
}

// The canvas a PNG header claims (its IHDR chunk, read from the first 24 bytes), for naming a refusal when the decoder will not give a size because
// the image is over its allocation limit. An invalid QSize when the bytes are not a PNG header.
inline QSize pngHeaderSize(const QByteArray &head)
{
    if (head.size() < 24 || !head.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)) || head.mid(12, 4) != "IHDR")
        return {};
    const quint32 w = qFromBigEndian<quint32>(head.constData() + 16), h = qFromBigEndian<quint32>(head.constData() + 20);
    return QSize(int(qMin<quint32>(w, 0x7fffffff)), int(qMin<quint32>(h, 0x7fffffff)));
}

} // namespace murasaki::import
