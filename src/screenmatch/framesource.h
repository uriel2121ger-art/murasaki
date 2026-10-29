#pragma once

// Where the helper's frames come from. In production the PipeWire stream of the portal's screen cast; in tests an in-process fake. It is an interface of the process and never a path or a
// setting a user can reach: the helper has no option that names a file to read frames from.

#include "screenmatch.h"

namespace murasaki::screenmatch {

struct Frame {
    int width = 0;
    int height = 0;
    qsizetype stride = 0;
    ScreenPixelFormat format = ScreenPixelFormat::Bgra32;
    const uchar *data = nullptr;
    qsizetype size = 0; // bytes readable at `data`
};

class FrameSource
{
public:
    virtual ~FrameSource() = default;
    virtual int streamCount() const = 0;
    // The newest frame of the stream, or false when it has none yet. Frames older than it are dropped unread (the newest-frame-only policy: a slow tick never works through a backlog). The frame
    // stays valid until release() for the same stream.
    virtual bool acquire(int stream, Frame *frame) = 0;
    virtual void release(int stream) = 0;
};

} // namespace murasaki::screenmatch
