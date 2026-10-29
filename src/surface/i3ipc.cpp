#include "i3ipc.h"

#include <QtEndian>

#include <cstring>

namespace murasaki {
namespace {

constexpr char kMagic[] = "i3-ipc";
constexpr qsizetype kMagicSize = 6;
constexpr qsizetype kHeaderSize = kMagicSize + 8;
constexpr quint32 kMaxPayload = 16 * 1024 * 1024;

} // namespace

QByteArray i3ipcFrame(quint32 type, const QByteArray &payload)
{
    QByteArray frame(kMagic, kMagicSize);
    const quint32 header[2] = {quint32(payload.size()), type};
    frame.append(reinterpret_cast<const char *>(header), sizeof(header));
    frame.append(payload);
    return frame;
}

QList<I3IpcMessage> i3ipcTake(QByteArray &buffer, bool *corrupt)
{
    if (corrupt)
        *corrupt = false;
    QList<I3IpcMessage> messages;
    qsizetype offset = 0;
    while (buffer.size() - offset >= kHeaderSize) {
        if (std::memcmp(buffer.constData() + offset, kMagic, kMagicSize) != 0) {
            buffer.clear();
            if (corrupt)
                *corrupt = true;
            return messages;
        }
        const quint32 length = qFromUnaligned<quint32>(buffer.constData() + offset + kMagicSize);
        const quint32 type = qFromUnaligned<quint32>(buffer.constData() + offset + kMagicSize + 4);
        if (length > kMaxPayload) {
            buffer.clear();
            if (corrupt)
                *corrupt = true;
            return messages;
        }
        if (buffer.size() - offset - kHeaderSize < qsizetype(length))
            break;
        messages.append({type, buffer.mid(offset + kHeaderSize, length)});
        offset += kHeaderSize + length;
    }
    buffer.remove(0, offset);
    return messages;
}

} // namespace murasaki
