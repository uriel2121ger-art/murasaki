#pragma once

#include "colorkey.h"
#include "framesink.h"

#include <QByteArray>

#include <cstring>

namespace murasaki::import {

// A FrameSink that applies the colour key to each frame and passes it on: the one way every source (sheet, images, zip, gif, video) gets the key.
// Disabled, it passes the frame through without a copy. The frame it is given is never modified.
class KeyedSink final : public FrameSink
{
public:
    KeyedSink(FrameSink &target, const ColorKey &key) : m_target(target), m_key(key) {}

    bool begin(int width, int height) override
    {
        m_width = width;
        m_height = height;
        return m_target.begin(width, height);
    }
    bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) override
    {
        if (!m_key.enabled)
            return m_target.addFrame(rgba, stride, delayMs);
        const qsizetype row = qsizetype(m_width) * 4;
        m_scratch.resize(row * m_height);
        for (int y = 0; y < m_height; ++y)
            std::memcpy(m_scratch.data() + row * y, rgba + stride * y, size_t(row));
        applyColorKey(reinterpret_cast<uchar *>(m_scratch.data()), m_width, m_height, row, m_key);
        return m_target.addFrame(reinterpret_cast<const uchar *>(m_scratch.constData()), row, delayMs);
    }
    bool finish() override { return m_target.finish(); }
    int frames() const override { return m_target.frames(); }
    long long bytesWritten() const override { return m_target.bytesWritten(); }
    QString error() const override { return m_target.error(); }
    bool overBudget() const override { return m_target.overBudget(); }

private:
    FrameSink &m_target;
    ColorKey m_key;
    int m_width = 0;
    int m_height = 0;
    QByteArray m_scratch;
};

} // namespace murasaki::import
