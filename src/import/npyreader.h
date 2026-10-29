#pragma once

// A strict reader of the captured frames of a Triggers pack: `frames/region<i>.npz`, a ZIP that holds one NumPy array `frames` of shape
// [frames, height, width, 3], unsigned 8-bit RGB. It reads the .npy format by its fixed grammar and nothing else: no object arrays (so no serialised program is ever loaded), no
// other dtype, no Fortran order, no other shape. Every limit is checked on the header, before any data is read, and the data is streamed: a frame is kept only when the
// caller asked for it, so memory stays at one frame whatever the array's size.

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

class QIODevice;

namespace murasaki::import {

struct NpyLimits {
    int maxFrames = 1024;
    int maxSide = 4096;                          // height and width
    long long maxDataBytes = 256LL << 20;        // frames x height x width x 3
    int maxHeaderBytes = 64 * 1024;
};

struct NpyShape {
    int frames = 0;
    int height = 0;
    int width = 0;
    long long frameBytes() const { return (long long)height * width * 3; }
    long long dataBytes() const { return frameBytes() * frames; }
};

struct NpyHeader {
    NpyShape shape;
    qsizetype dataOffset = 0; // preamble and header, in bytes
};

// Parses the start of a .npy file (as many bytes as are available: the magic, the version, the length and the header) with the limits. nullopt with *error set to a name of
// petimport.h (kNpy...) followed by ": detail" when the header is refused; *needMore is set instead (and *error left empty) when `prefix` ends before the header does.
// This is the whole format reader: the dictionary is parsed by a small grammar, nothing is evaluated.
std::optional<NpyHeader> parseNpyHeader(const QByteArray &prefix, const NpyLimits &limits, QString *error, bool *needMore = nullptr);

// Reads an array as it streams in. Feed it the bytes of a .npy in any chunking; after the header it calls `choose` once with the shape and gets back the frame indices to keep
// (any order, repeats ignored; an index outside the array is a refusal of the caller's, reported as kNpyShape). `deliver` gets each kept frame once, in index order, as
// width x height x 3 RGB bytes, valid during the call. `knownSize` is the file's size when it is known (a ZIP member's declared size, a file's size): then a size that
// disagrees with the shape is refused as truncated or as followed by data as soon as the header is read, before any pixel.
class NpyFrameReader
{
public:
    using Choose = std::function<QList<int>(const NpyShape &)>;
    using Deliver = std::function<bool(int index, const uchar *rgb, int width, int height)>;

    NpyFrameReader(const NpyLimits &limits, Choose choose, Deliver deliver, long long knownSize = -1);

    // Asked at every feed(); false stops the read with `cancelled` (a cancelled or timed-out import).
    void setAlive(std::function<bool()> alive) { m_alive = std::move(alive); }
    bool feed(const char *data, qsizetype length); // false once refused; error() says why
    bool finish();                                 // false when the array is not complete (kNpyTruncated) or something is wrong
    QString error() const { return m_error; }
    bool hasShape() const { return m_haveHeader; }
    const NpyShape &shape() const { return m_shape; }

private:
    bool fail(const QString &why);

    NpyLimits m_limits;
    Choose m_choose;
    Deliver m_deliver;
    std::function<bool()> m_alive;
    long long m_knownSize;
    QByteArray m_head;
    bool m_haveHeader = false;
    bool m_failed = false;
    NpyShape m_shape;
    QList<int> m_wanted; // sorted, unique
    qsizetype m_nextWanted = 0;
    long long m_consumed = 0; // data bytes seen
    QByteArray m_frame;       // the frame being filled when it is wanted
    QString m_error;
};

// A plain .npy from a device, read to its end. `shape` (optional) is filled once the header is known.
bool readNpyFrames(QIODevice *device, const NpyLimits &limits, const NpyFrameReader::Choose &choose, const NpyFrameReader::Deliver &deliver, QString *error, NpyShape *shape = nullptr,
                   const std::function<bool()> &alive = {});

// A .npz: a ZIP (read with BoundedZip, stored or deflate, with its own ratio and size caps) whose member `frames.npy` is read as above.
bool readNpzFrames(QIODevice *device, const NpyLimits &limits, const NpyFrameReader::Choose &choose, const NpyFrameReader::Deliver &deliver, QString *error, NpyShape *shape = nullptr,
                   const std::function<bool()> &alive = {});

} // namespace murasaki::import
