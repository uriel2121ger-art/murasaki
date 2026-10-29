#pragma once

#include <QByteArray>
#include <QList>

namespace murasaki {

// i3/Sway IPC: "i3-ipc" + payload length + type (both 32-bit, host byte order) + payload.
inline constexpr quint32 kI3IpcEventBit = 0x80000000u;

struct I3IpcMessage {
    quint32 type = 0; // events have kI3IpcEventBit set
    QByteArray payload;
};

QByteArray i3ipcFrame(quint32 type, const QByteArray &payload);

// Removes every complete message from the front of buffer. Data without the magic prefix, or a length over the
// limit, clears the buffer and sets *corrupt (when given): the stream is desynchronised and the caller should
// reconnect. Messages completed before the corruption are still returned.
QList<I3IpcMessage> i3ipcTake(QByteArray &buffer, bool *corrupt = nullptr);

} // namespace murasaki
