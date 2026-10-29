#pragma once

#include "framesink.h"

#include <QByteArray>
#include <QString>

class QIODevice;

namespace murasaki::import {

// Writes an APNG one frame at a time, so that memory is one frame whatever the length: IHDR, acTL (patched with the real frame count at the end),
// then for every frame an fcTL and its pixels as IDAT (the first) or fdAT chunks of at most 256 KiB. Frames are full-canvas, 8-bit RGBA with straight
// (not premultiplied) alpha, blend op source and dispose op none: no inter-frame optimisation, simple and exact. Rows are PNG-filtered (none, sub or up,
// by the usual smallest-sum rule) and deflated with zlib. The device must be seekable.
//
// Every call checks its limits before it allocates: the canvas side (kMaxCanvasSide), the frame count (kMaxFrames) and the bytes written (`maxBytes`).
class ApngWriter : public FrameSink
{
public:
    explicit ApngWriter(QIODevice *device, long long maxBytes = 1LL << 30);

    // Starts the file. False with error() set for a canvas outside 1..kMaxCanvasSide per side or a device that cannot be written.
    bool begin(int width, int height) override;
    // Adds a frame of width x height RGBA pixels, `stride` bytes between rows (>= width * 4), shown for delayMs (0 to 655350; written as num/den, milliseconds up to 65535 ms and
    // centiseconds above, longer ones are clamped to 655350). False with error() set once a limit is hit or the device fails; the writer is then dead.
    bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) override;
    // Writes IEND and patches the frame count. At least one frame is needed.
    bool finish() override;

    int frames() const override { return m_frames; }
    long long bytesWritten() const override { return m_written; }
    QString error() const override { return m_error; }
    bool overBudget() const override { return m_overBudget; }

private:
    bool write(const QByteArray &bytes);
    bool writeChunk(const char *type, const QByteArray &data);
    bool fail(const QString &message);

    QIODevice *m_device;
    long long m_maxBytes;
    int m_width = 0;
    int m_height = 0;
    int m_frames = 0;
    quint32 m_sequence = 0;
    qint64 m_actlOffset = -1; // where the acTL data starts
    long long m_written = 0;
    bool m_begun = false;
    bool m_dead = false;
    bool m_overBudget = false;
    QString m_error;
};

} // namespace murasaki::import
