#pragma once

#include "framesink.h"

class QIODevice;

namespace murasaki::import {

// An animated WebP through libwebpmux's WebPAnimEncoder (built only when the library is there: webpOutputAvailable()). Lossless and exact (colours under
// transparent pixels are kept) at quality 100, lossy below. The encoder keeps the encoded frames in memory until finish(), so the canvas, frame count and
// the budget are checked on every frame against the raw size, and the finished file against maxBytes.
class WebpWriter : public FrameSink
{
public:
    WebpWriter(QIODevice *device, int quality, long long maxBytes);
    ~WebpWriter() override;
    WebpWriter(const WebpWriter &) = delete;
    WebpWriter &operator=(const WebpWriter &) = delete;

    static bool webpOutputAvailable();

    bool begin(int width, int height) override;
    bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) override;
    bool finish() override;
    int frames() const override { return m_frames; }
    long long bytesWritten() const override { return m_written; }
    QString error() const override { return m_error; }
    bool overBudget() const override { return m_overBudget; }

private:
    bool fail(const QString &message);

    struct Impl;
    Impl *m_impl = nullptr;
    QIODevice *m_device;
    int m_quality;
    long long m_maxBytes;
    int m_width = 0;
    int m_height = 0;
    int m_frames = 0;
    long long m_timestamp = 0;
    long long m_written = 0;
    long long m_rawHeld = 0;
    bool m_begun = false;
    bool m_dead = false;
    bool m_overBudget = false;
    QString m_error;
};

} // namespace murasaki::import
