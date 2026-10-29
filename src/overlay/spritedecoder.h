#pragma once

#include "alphamask.h"
#include "petlimits.h"

#include <QImage>
#include <QList>
#include <QMap>
#include <QSize>
#include <QString>

#include <atomic>

namespace murasaki {

// The decoded frames of one animation, scaled to the size they are drawn at. Every image is
// Format_RGBA8888_Premultiplied, which is what the scene graph uploads without a conversion.
struct SpriteFrames {
    QList<QImage> images;
    QList<int> durationsMs; // how long each frame shows (the file's delays by petFrameDelayMs); same length as images, each at least kMinFrameMs
    QSize size;       // of the whole frame (the pet's picture), whatever the images hold
    qint64 bytes = 0; // memory of all images
    // Where each image sits in the frame, when they were cut down to what is not transparent: all or none - empty (every image is the whole frame) or
    // one per image. A set where it is neither is refused by the item (consistent()).
    QList<QPoint> offsets;

    bool isEmpty() const { return images.isEmpty(); }
    bool consistent() const { return offsets.isEmpty() || offsets.size() == images.size(); }
    // Keeps of each image the rectangle with something in it (one transparent pixel more on every side that has room, so that a scaled
    // draw blends with transparency at the edge as the whole image did). A frame with nothing in it becomes one transparent pixel.
    // The animation is the same; the memory (and the textures on the GPU) is what is drawn, not the margins. Images that are
    // not Format_RGBA8888_Premultiplied are left as they are. Meant for a worker thread.
    void cropTransparentMargins();
};

// Decodes a whole animation (APNG, GIF, or the WebM with alpha the transcoder writes) with libavformat and
// libavcodec, and an animated WebP with Qt's WebP plugin (FFmpeg's decoder reads only still ones), once, so that
// it can be played from textures afterwards. The file is opened through the "file:" protocol only, with the
// container forced from the extension, like the transcoder does: files come from the Workshop. Everything here is thread-safe (no shared state) and meant to run on a worker thread.
class SpriteDecoder
{
public:
    static constexpr int kMinFrameMs = 10; // the floor of a retimed WebM's frames and of fixedDurations(); the pets' own fixed rate (FrameClock) has none
    static constexpr int kMaxFrames = kMaxPetFrames;
    static constexpr int kMaxSide = kMaxPetCanvasSide; // the largest canvas, and the largest size frames are made at

    // Asks decode() to make the hit masks of the frames while it decodes them (a callback on each converted frame, no second
    // decode). The masks are the picture's own size, built inside `budgetBytes` (MaskBuilder: the grid is lowered, then only the
    // union is kept). With keepFrames false the frames are dropped as soon as their mask is made: memory is the masks, and the
    // frame cache budget does not apply (the low-resolution pass for pets that play through libmpv).
    struct MaskRequest {
        std::size_t budgetBytes = kMaskBudgetBytes;
        bool keepFrames = true;
    };

    struct Result {
        bool ok = false;
        bool cancelled = false;
        QString error; // when !ok and !cancelled
        SpriteFrames frames;
        qint64 decodedFrames = 0; // frames decoded, also when keepFrames is false
        MaskSet masks;            // when a MaskRequest was given and the decode succeeded; null otherwise (a cancelled or failed decode leaves none)
    };

    // Bytes the animation takes when decoded at `target` (an empty target means the file's own size), from the
    // container header alone; 0 when the header does not tell how many frames there are. `error` is set when the
    // file cannot be read at all.
    static qint64 estimateBytes(const QString &file, QSize target, QString *error = nullptr);

    // The durations of `frames` frames that step at a fixed rate: 1000/fps ms each, rounded so that they add up
    // (60 fps: 17, 17, 16, ...). Each is at least kMinFrameMs.
    static QList<int> fixedDurations(int frames, int fps);

    // Decodes the animation scaled to `target` (empty: its own size). Fails before decoding when the header says
    // the frames would take more than maxBytes, and during decoding as soon as they do; a cancel flag that turns
    // true stops it at the next frame. The decoder libvpx-vp9 is required for VP9, because the native one drops
    // the alpha plane.
    // With fixedFps > 0 every frame lasts 1/fixedFps seconds (Anima Engine steps its pets at a fixed rate and
    // ignores the file's delays) instead of what the file says.
    // With cropMargins every frame is cut to what is drawn as it is decoded (SpriteFrames::cropTransparentMargins), and maxBytes
    // counts what is kept: a pet whose cut frames fit is not refused for the size of its whole ones.
    static Result decode(const QString &file, QSize target, qint64 maxBytes, const std::atomic_bool *cancel = nullptr,
                         int fixedFps = 0, const MaskRequest *masks = nullptr, bool cropMargins = false);

    // The mask pass for a pet that plays through libmpv and has no pixels in the GUI: decodes `file` in a stream, one frame in memory at a
    // time, and keeps only the masks. The frames are converted at the file's own size (at most kMaskPassMaxPixels: a larger canvas is scaled
    // down to fit, which can thin a very fine feature), so the mask is made from the same alpha values as the frame cache path makes it,
    // threshold included: a one-pixel line keeps its cell. `result.frames` is empty; `result.masks` and `result.decodedFrames` are the
    // answer; `size` is the picture the masks describe.
    static constexpr qint64 kMaskPassMaxPixels = 16LL << 20; // one converted frame is at most 64 MiB
    struct MaskPassResult {
        Result result;
        QSize size;
    };
    static MaskPassResult decodeMasks(const QString &file, const std::atomic_bool *cancel = nullptr,
                                      std::size_t budgetBytes = kMaskBudgetBytes);
    // The size the mask pass decodes a picture of `native` at: its own, or scaled down (aspect kept) to at most maxPixels.
    static QSize maskPassSize(QSize native, qint64 maxPixels = kMaskPassMaxPixels);
    // The picture size of the file from its header (empty when it cannot be read).
    static QSize nativeSize(const QString &file);

    // What a streaming read of a file found (the pets sweep, tools/compat/pets_sweep.py): the frames the decoder produced, the span of their
    // presentation times and a few chosen frames, at the file's own size, premultiplied. Only the chosen frames and the last one are kept and
    // converted, so a 286 MiB APNG of 868 big frames costs a couple of frames of memory, not the whole animation.
    struct Probe {
        bool ok = false;
        QString error;
        qint64 frames = 0;      // frames the decoder produced
        double durationMs = 0;  // from the first frame's presentation time to the end of the last (its own duration, else the average step)
        QSize size;
        qint64 resizedFrames = 0; // frames whose own size is not the stream's (they are scaled to it: not hidden, counted)
        QSize otherSize;          // the first such frame's size
        QMap<qint64, QImage> images; // by index, the ones asked for (and the last, under index frames - 1)
    };
    // The most memory a probe may need: one canvas for each frame kept (those asked for, and the last) and one being converted.
    static constexpr qint64 kProbeMaxBytes = kMaxPetProbeBytes;
    // Decodes the whole file in a stream, keeping the frames at `indexes` and the last one. Counts and times come from the decode, so they
    // can be set against the container's header (promisedFrames) and an independent reader's.
    static Probe probe(const QString &file, const QList<qint64> &indexes, qint64 maxBytes = kProbeMaxBytes,
                       const std::atomic_bool *cancel = nullptr); // (the cancel flag and the frame limit are checked per frame for WebP)
    // The frame count the container header promises, 0 when it does not say; `error` is set when the file cannot be opened.
    static qint64 promisedFrames(const QString &file, QString *error = nullptr);
};

} // namespace murasaki
