#pragma once

// What the helper believes about a buffer a PipeWire server hands over, decided in one pure function (no PipeWire, no memory read) so that every refusal is tested. The server is not
// trusted: its chunk (offset, size, stride, flags) and the mapping it claims are checked against what was negotiated before one byte is read.

#include "framesource.h"

namespace murasaki::screenmatch {

struct BufferInput {
    bool dmaBuf = false;         // the data block is a dma-buf
    bool allowDmaBuf = false;    // the helper was started with --buffers any
    const void *data = nullptr;  // the mapping; null or MAP_FAILED when the map did not work
    bool mapFailed = false;      // data is MAP_FAILED
    bool hasChunk = false;
    bool corrupted = false;      // SPA_CHUNK_FLAG_CORRUPTED
    qint64 offset = 0;           // the chunk, as the server wrote it (wider than the wire types so a wrap is visible)
    qint64 size = 0;
    qint64 stride = 0;
    qint64 maxSize = 0;          // the size of the mapping
    bool formatKnown = false;    // the negotiated format is one of ours
    ScreenPixelFormat format = ScreenPixelFormat::Bgra32;
    qint64 width = 0;            // the negotiated size, now (a format change mid-stream changes these between two buffers)
    qint64 height = 0;
};

enum class BufferVerdict { Ok, NoMapping, NoChunk, Corrupted, DmaBufRefused, UnknownFormat, BadSize, BadOffset, BadStride, TooSmall };

// Ok fills *frame (the pointer is data + offset); anything else leaves it alone.
BufferVerdict checkBuffer(const BufferInput &in, Frame *frame);

} // namespace murasaki::screenmatch
