#pragma once

#include <QString>

namespace murasaki::import {

// Where an import's frames go: an APNG written as it comes, or an animated WebP assembled at the end. Full-canvas RGBA8888, straight alpha.
class FrameSink
{
public:
    virtual ~FrameSink() = default;
    virtual bool begin(int width, int height) = 0;
    virtual bool addFrame(const uchar *rgba, qsizetype stride, int delayMs) = 0;
    virtual bool finish() = 0;
    virtual int frames() const = 0;
    virtual long long bytesWritten() const = 0;
    virtual QString error() const = 0;
    // True when the failure was the byte budget (the job names it kOutputOverBudget).
    virtual bool overBudget() const = 0;
};

} // namespace murasaki::import
