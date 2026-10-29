#pragma once

#include "alphamask.h"
#include "pettiming.h"
#include "spritedecoder.h"

#include <QElapsedTimer>
#include <QQuickItem>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>

namespace murasaki {

namespace scene {
class FpsController;
}

// Plays an animation from textures: the frames are decoded once on a worker thread (SpriteDecoder), uploaded to
// the GPU a few per rendered frame, and the CPU copies are freed. Playing is then just switching the texture at
// the animation's pace (the file's delays, a fixed rate or a speed multiplier: PetTiming, changed live without
// decoding again), and the item redraws only when the frame changes, so a still-looking pet costs no continuous
// rendering. Which frame shows is decided by the wall clock (FrameClock): a frame that is not drawn in time is
// skipped, the animation never slows down. fpsCap limits how often the frame may change, and the item lowers that
// limit by itself while its timer fires late (the machine cannot keep up). Needs a window with a scene graph (the textures belong to its render thread).
//
// request() starts decoding a file at a pixel size. The frames already shown keep playing until the new ones are
// uploaded, then they are swapped without a gap. loaded() says the new frames are on screen, failed() that they
// could not be made (the old ones are dropped then: the caller falls back to something else). clear() drops
// everything. hasFrames tells whether any texture is shown, old or new.
class SpriteItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool drawn READ drawn WRITE setDrawn NOTIFY drawnChanged)
    Q_PROPERTY(bool hasFrames READ hasFrames NOTIFY hasFramesChanged)
    Q_PROPERTY(double cacheBytes READ cacheBytesValue NOTIFY hasFramesChanged)
    Q_PROPERTY(int timingFps READ timingFps WRITE setTimingFps NOTIFY timingChanged)
    Q_PROPERTY(double timingSpeed READ timingSpeed WRITE setTimingSpeed NOTIFY timingChanged)
    Q_PROPERTY(int fpsCap READ fpsCap WRITE setFpsCap NOTIFY fpsCapChanged)
    Q_PROPERTY(int currentFrame READ currentFrame NOTIFY frameChanged)

public:
    explicit SpriteItem(QQuickItem *parent = nullptr);
    ~SpriteItem() override;

    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    // Whether the frames are drawn. Not drawn, the item still decodes, makes its textures and keeps its state: it is the set of frames prepared behind the one on
    // screen (docs/pets/states.md, the preload), and draws nothing until it takes the other's place.
    bool drawn() const { return m_drawn; }
    void setDrawn(bool drawn);
    bool hasFrames() const { return m_hasFrames; }
    qint64 cacheBytes() const { return m_cacheBytes; }
    double cacheBytesValue() const { return double(m_cacheBytes); }
    int frameCount() const { return m_durations.size(); }
    int currentFrame() const { return m_frame; }
    int frameDuration(int index) const { return index >= 0 && index < m_durations.size() ? m_durations.at(index) : 0; }
    // The masks of the frames on screen (null: none were asked for, or the frames are not there). Index i is frame i, currentFrame().
    std::shared_ptr<const MaskSet> masks() const { return m_masks; }
    // Decoded images the item still holds on the CPU side (waiting for their upload); 0 once the frames are on the
    // GPU.
    int pendingCpuFrames() const;
    bool loading() const { return m_generation != m_settledGeneration; }

    int timingFps() const { return m_frames.timing().fps; }
    double timingSpeed() const { return m_frames.timing().speed; }
    void setTimingFps(int fps);
    void setTimingSpeed(double speed);
    // The most frames per second the frame may change at, from the outside (the daemon's setting, the power state);
    // 0: no limit.
    int fpsCap() const { return m_fpsCap; }
    void setFpsCap(int fps);
    // The limit in force: fpsCap, lowered by the item itself when it falls behind. 0: none.
    int effectiveFpsCap() const { return m_frames.cap(); }
    // Tells the item how late its timer fired (ms); what tick() does with each frame. Public for the tests.
    void noteLateness(double lateMs);

    // Decodes `file` scaled to width x height pixels, refusing animations of more than maxBytes. With fps > 0 the
    // frames are decoded with that rate for their delays (the pets do not use this: they set the timing instead).
    // With maskBudget > 0 the hit masks of the frames are made while they are decoded (SpriteDecoder::MaskRequest) within that many bytes;
    // masks() has them once loaded() says the frames are on screen.
    Q_INVOKABLE void request(const QString &file, int width, int height, double maxBytes, int fps = 0, double maskBudget = 0);
    Q_INVOKABLE void clear();
    // Plays from the first frame again (the frames stay): a click on an animation that plays once.
    Q_INVOKABLE void restart();
    // The libmpv video of this pet already announced its loop 0 (and its sound played): the loop 0 of the frames that replace it is not announced, so that
    // the handover does not play the sound twice. Used once, by the next frames that are shown.
    Q_INVOKABLE void skipFirstWrap() { m_skipFirstWrap = true; }
    // Shows frames that are already decoded, as if request() had produced them. For tests.
    void showFrames(SpriteFrames frames);
    // The one thread all pets decode on. For tests, which keep it busy to see that nothing waits for it.
    static QThreadPool &workerPool();

signals:
    void pausedChanged();
    void drawnChanged();
    void timingChanged();
    void fpsCapChanged();
    void hasFramesChanged();
    // The frame on screen changed (the index into the decoded frames, which is also the index of its mask).
    void frameChanged();
    // The animation wraps to its first frame at `monotonicNs` (CLOCK_MONOTONIC), the exact time under the clock's timing: announced up to 150 ms ahead (or
    // at the start of the last frame when frames are longer than that) so that what depends on it can be scheduled to the sample whatever this thread's
    // timer jitter is. The first showing is the loop 0, announced when it begins. A wrap that was missed (the thread stalled past it) is announced late with
    // its exact time, once, for the latest.
    void wrapped(qint64 monotonicNs, qint64 loop);
    // The announced wrap will not happen at that time (a pause, a change of timing, new frames): drop it. A new announcement follows when the clock runs.
    void wrapCancelled();
    // The frames of the last request are on screen; bytes is what they take on the GPU.
    void loaded(double bytes);
    void failed(const QString &reason);

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    // Between the item and its decoding tasks. A task delivers its result through it; the destructor cuts it, so
    // the item never waits for the tasks (which may be queued behind another pet's decode) and they never touch it
    // after it is gone.
    struct Link;

    void onDecoded(quint64 generation, SpriteDecoder::Result result);
    void adopt(SpriteFrames frames);
    void afterUpload(quint64 generation);
    void cancelDecoding();
    void scheduleNext();
    void tick();
    double nowMs() const { return double(m_wall.nsecsElapsed()) / 1e6; }
    void applyEffectiveCap();

    // Written by the GUI thread outside of the scene graph's sync step and by updatePaintNode during it (when the
    // GUI thread is blocked), so never concurrently.
    std::shared_ptr<const MaskSet> m_incomingMasks; // of m_incoming, handed to m_masks when its frames are on screen
    std::shared_ptr<const MaskSet> m_masks;
    SpriteFrames m_incoming;       // decoded, being uploaded; images are released as they go
    int m_uploaded = 0;
    bool m_resetIncoming = false;  // the node must forget the textures it uploaded for an earlier request
    bool m_drop = false;           // the node must drop its textures
    QList<int> m_durations;        // of the frames in the node
    int m_frame = 0;
    qint64 m_cacheBytes = 0;
    QSize m_frameSize;
    bool m_hasFrames = false;
    bool m_hasFramesReported = false; // what hasFramesChanged last said
    bool m_paused = false;
    bool m_drawn = true;

    quint64 m_generation = 0;      // of the last request
    quint64 m_settledGeneration = 0; // the last request that finished (loaded or failed)
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<Link> m_link;
    QTimer m_timer;
    QElapsedTimer m_wall;
    qint64 m_wallStartNs = 0;  // CLOCK_MONOTONIC when m_wall started, so a wall time of the clock is a time other processes can read
    qint64 m_loopAnnounced = -1; // the last loop whose wrap wrapped() announced
    bool m_skipFirstWrap = false;
    qint64 wrapNs(qint64 loop) const { return m_wallStartNs + qint64(m_frames.wrapTimeMs(loop) * 1e6); }
    void cancelWrapAnnouncement(); // after a pause or a change of timing: forget the announcement past the clock's own count
    FrameClock m_frames;
    double m_dueMs = -1;        // when the timer was meant to fire
    int m_fpsCap = 0;
    std::unique_ptr<scene::FpsController> m_adaptive; // lowers the cap while the timer fires late
};

} // namespace murasaki
