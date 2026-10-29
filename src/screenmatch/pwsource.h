#pragma once

// The PipeWire side of the helper: one consumer stream per monitor node of the portal's screen cast, on a remote the daemon opened (OpenPipeWireRemote) and passed as an inherited
// descriptor. The helper never connects to anything itself. Single threaded: the caller polls pollFd() and calls iterate(); a stream's `process` runs inside iterate().
//
// Newest frame only: when several buffers are queued the older ones go straight back to the server unread, and the one kept is held (not returned) until a newer one replaces it, so a still
// screen, which sends no new frames, is scored again from the last one. Buffers are SHM (memfd or pointer) unless `allowDmaBuf` is set, in which case a dma-buf is mapped and read as a linear
// buffer (a tiled one would give garbage, never a crash: the reads are bounded by the chunk the server declared and the mapping).

#include "framesource.h"

#include <QString>

#include <memory>
#include <vector>

namespace murasaki::screenmatch {

class PipeWireFrameSource : public FrameSource
{
public:
    ~PipeWireFrameSource() override;

    // `pwFd` is owned from here on. nullptr with *error when PipeWire cannot be started or the descriptor is not a remote. `fps` is the highest rate the streams ask for.
    static std::unique_ptr<PipeWireFrameSource> create(int pwFd, const std::vector<unsigned> &nodes, int fps, bool allowDmaBuf, QString *error);

    int pollFd() const;        // readable when iterate() has work
    void iterate();            // runs the PipeWire loop without blocking
    bool failed() const;       // the remote or a stream ended (the desktop stopped the share)
    QString failure() const;

    int streamCount() const override;
    bool acquire(int stream, Frame *frame) override;
    void release(int stream) override;

    struct Impl; // (public only so that the PipeWire callbacks in the .cpp can name it)

private:
    PipeWireFrameSource();
    std::unique_ptr<Impl> m_impl;
};

} // namespace murasaki::screenmatch
