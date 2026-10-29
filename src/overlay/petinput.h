#pragma once

// The pointer input of one pet window, without the window: PetInteraction (hide, fade, dodge, drag) fed by the pointer events and the
// global pointer, the hit mask of the frame on screen, and what follows from them for the window: the opacity factor, the input region
// as rectangles in window pixels, the layout (small, or grown for a drag) and the steps the pet should take. PetWindow forwards the events
// and applies the outputs; the controller tests drive this directly. Coordinates: the pet's window rectangle and the output are in the
// output's logical pixels (the pet's own x and y are the window's top left, rotation aside); `pos` of an event is relative to the window,
// or to the output when the window covers it (`windowIsOutput`).

#include "alphamask.h"
#include "petbehaviours.h"
#include "petgeometry.h"
#include "petinteraction.h"
#include "pointersource.h"

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <memory>
#include <string_view>
#include <vector>

namespace murasaki {

// What the pointer did to the pet, as the states of the pet need it (docs/pets/states.md; PetStateMachine): a drag began, the pet followed the pointer, the
// button was let go, or a dodge step was taken. The numbers are the step in output pixels (DragMove, Dodge) and how long the step shows `moving` (Dodge).
struct PetPointerEvent {
    enum class Kind { Grab, DragMove, Release, Dodge };
    Kind kind = Kind::Grab;
    double dx = 0;
    double dy = 0;
    int durationMs = 0;
};

struct PetInputStatus {
    QString pointer;                          // "global" or "local"
    QString hitMask;                          // "frame", "silhouette" or "none"
    qint64 maskBytes = 0;
    bool hidden = false;                      // faded by hover or focus
    std::vector<std::string_view> degraded;   // names of petbehaviours.h that apply now
};

class PetInput : public QObject
{
    Q_OBJECT

public:
    static constexpr int kTickMs = 32;            // Anima's interaction tick
    static constexpr int kRegionMinIntervalMs = 33; // a region changes at most 30 times a second

    explicit PetInput(QObject *parent = nullptr);

    // ---- configuration ----
    // Off while the arrange mode (or nothing at all) edits the pet: nothing is computed, the outputs are neutral.
    void setEnabled(bool on);
    bool enabled() const { return m_enabled; }
    void setLevel(PointerSource::Level level);
    void setBehaviour(const PetBehaviourConfig &config);
    // The pet's window rectangle and its output, both in the output's logical pixels.
    void setGeometry(PetRect window, PetSize output);
    // The masks of the picture on screen. With frameTracking the current frame's mask is used (setFrame); without (the video path: the
    // frame on screen is not known) the union of all the frames is.
    void setMasks(std::shared_ptr<const MaskSet> masks, bool frameTracking);
    void setFrame(int frame);
    // Rotation and flips already applied to the picture (the window is the footprint): the masks are turned the same way.
    void setTransform(double rotation, bool flipH, bool flipV);

    // ---- events ----
    void enter(QPointF pos, bool windowIsOutput);
    void leave();
    void motion(QPointF pos, bool windowIsOutput);
    void press(QPointF pos, bool windowIsOutput);
    void release();
    // The global cursor (PointerSource::moved), already in this pet's output logical pixels.
    void globalPointer(QPointF position);

    // ---- outputs ----
    // Whether the global pointer source has to run for this pet.
    bool wantsGlobalPointer() const;
    // Any behaviour on: the pet needs the pointer (global) or its own events (local).
    bool needsPointer() const;
    double opacityFactor() const;
    InputRegion region() const;
    PetLayout layout() const;
    // A click went through the faded pet: the window unmaps until the release ends (PetInteraction::released).
    bool released() const;
    // Relative pointer motion (RelativePointer): only a drag reads it.
    void relativeMotion(double dx, double dy);
    // The input region for the current frame, in window pixels: rectangles of the mask (<= 128 for a frame, <= 512 for the union), the
    // whole window when there is no mask yet, none when the region is empty.
    const std::vector<MaskRect> &regionRects();
    PetInputStatus status() const;

signals:
    // Opacity, region kind, layout or the region itself changed.
    void changed();
    // The pet should move by this much (a dodge, or the drag following the pointer); the owner applies it and calls setGeometry().
    void moveRequested(double dx, double dy);
    // A press and release on the pet that was not a drag (PetInteraction::takeClick). Comes after the Release of pointerEvent.
    void clicked();
    // The events the states of the pet read; nothing listens unless the pet has states.
    void pointerEvent(const murasaki::PetPointerEvent &event);

private:
    qint64 now() const { return m_clock.elapsed(); }
    QPointF local(QPointF pos, bool windowIsOutput) const;
    void afterInput();
    void updateTimer();
    void effectiveMaskChanged();
    std::shared_ptr<const AlphaMask> effectiveMask() const;
    void computeRects();
    void scheduleRegion();
    void emitIfChanged(bool force);

    PetInteraction m_interaction;
    QElapsedTimer m_clock;
    QTimer m_tick;
    QTimer m_regionTimer;
    bool m_enabled = true;
    PointerSource::Level m_level = PointerSource::Level::Global;
    PetBehaviourConfig m_config;
    PetRect m_window;
    PetSize m_output;
    std::shared_ptr<const MaskSet> m_masks;
    bool m_frameTracking = true;
    int m_frame = 0;
    double m_rotation = 0;
    bool m_flipH = false;
    bool m_flipV = false;
    mutable std::vector<std::pair<int, std::shared_ptr<const AlphaMask>>> m_turned; // transformed masks by frame (-1: the union)
    std::vector<MaskRect> m_rects;
    bool m_rectsDirty = true;
    qint64 m_lastRegionAt = -1000;
    // What the owner last heard about.
    double m_lastOpacity = 1;
    InputRegion m_lastRegion = InputRegion::Empty;
    PetLayout m_lastLayout = PetLayout::Small;
    bool m_lastReleased = false;
    std::vector<MaskRect> m_lastRects;
};

} // namespace murasaki

Q_DECLARE_METATYPE(murasaki::PetPointerEvent)
