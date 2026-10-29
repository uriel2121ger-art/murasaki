#include "bufferguard.h"

namespace murasaki::screenmatch {

BufferVerdict checkBuffer(const BufferInput &in, Frame *frame)
{
    if (in.dmaBuf && !in.allowDmaBuf)
        return BufferVerdict::DmaBufRefused; // asked for shared memory only: a server that hands over a dma-buf anyway is not read
    if (!in.data || in.mapFailed)
        return BufferVerdict::NoMapping;
    if (!in.hasChunk)
        return BufferVerdict::NoChunk;
    if (in.corrupted)
        return BufferVerdict::Corrupted;
    if (!in.formatKnown)
        return BufferVerdict::UnknownFormat;
    if (in.width <= 0 || in.height <= 0 || in.width > 16384 || in.height > 16384)
        return BufferVerdict::BadSize;
    const qint64 bpp = in.format == ScreenPixelFormat::Rgb24 ? 3 : 4;
    const qint64 row = in.width * bpp;
    // Everything below is 64 bit and every operand is bounded first, so no sum wraps.
    if (in.maxSize <= 0 || in.maxSize > (qint64(1) << 40) || in.offset < 0 || in.offset > in.maxSize || in.size <= 0 || in.size > in.maxSize || in.offset + in.size > in.maxSize)
        return BufferVerdict::BadOffset;
    if (in.stride < row || in.stride > (qint64(1) << 31))
        return BufferVerdict::BadStride;
    if ((in.height - 1) * in.stride + row > in.size)
        return BufferVerdict::TooSmall; // the last row would end outside the declared chunk
    frame->width = int(in.width);
    frame->height = int(in.height);
    frame->stride = qsizetype(in.stride);
    frame->format = in.format;
    frame->data = static_cast<const uchar *>(in.data) + in.offset;
    frame->size = qsizetype(in.size);
    return BufferVerdict::Ok;
}

} // namespace murasaki::screenmatch
