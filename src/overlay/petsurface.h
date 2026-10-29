#pragma once

#include "alphamask.h"
#include "petinput.h"
#include "petstore.h"
#include "pointersource.h"

#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QString>

#include <algorithm>
#include <functional>
#include <memory>

namespace murasaki {

// The wheel and the corner handle scale a pet between these multiples of its native size.
inline constexpr double kMinPetScale = 0.1;
inline constexpr double kMaxPetScale = 2.0;

// The window flags of a pet: never focused, click-through unless the arrange mode is on. (QWindow::setMask() with
// an empty region does not make a layer-shell surface click-through; Qt::WindowTransparentForInput does, because
// the Wayland plugin turns it into an empty input region, and it can be flipped at run time.)
inline Qt::WindowFlags petWindowFlags(bool arrange)
{
    Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;
    if (!arrange)
        flags |= Qt::WindowTransparentForInput;
    return flags;
}

// The flags of an X11 pet window (and so of a stage): the pet's, and no window manager (override-redirect) and the stay-on-top hint. PetWindow and the X11 tests use this one.
inline Qt::WindowFlags petX11WindowFlags(bool arrange)
{
    return petWindowFlags(arrange) | Qt::X11BypassWindowManagerHint | Qt::WindowStaysOnTopHint;
}

// The displayed width limited to 0.1x..2x of the native width (and at least the daemon's minimum).
inline double clampPetWidth(double width, int nativeWidth)
{
    const double low = std::max(kMinPetWidth, kMinPetScale * nativeWidth);
    const double high = std::max(low, std::min(kMaxPetWidth, kMaxPetScale * nativeWidth));
    return std::clamp(width, low, high);
}

// The window size of a pet shown `width` wide: the height follows the native aspect ratio.
inline QSize petDisplaySize(double width, QSize native)
{
    const int w = std::max(1, int(width + 0.5));
    if (native.width() < 1 || native.height() < 1)
        return {w, w};
    return {w, std::max(1, int(width * native.height() / native.width() + 0.5))};
}

// Whether a pet has any use for hit masks: it takes the pointer on its opaque pixels, or hides, fades or dodges by them. A click-through pet with
// none of those (the default, and `locked` alone) never makes, loads or keeps a mask: no decode work, no pass over its file, no cache file, no memory.
inline bool needsMasks(const PetPlacement &placement)
{
    if (placement.stage.active())
        return false; // a stage takes no pointer (docs/pets/stage.md): no mask pass, no decode of masks, no cache file
    return !placement.clickThrough || placement.hoverHide || placement.focusFade || placement.smartDodge;
}

// What a pet window shows. The controller computes it from the daemon's placement, the item and the policy.
struct PetView {
    // The fullscreen mode (docs/pets/stage.md): the window covers its output, takes no input and draws the animation in `petRect` (the unrotated
    // rectangle, in the window's coordinates; rotation and flips are the placement's). Inactive: an ordinary pet.
    struct Stage {
        bool active = false;
        QRectF petRect;
    };
    Stage stage;
    PetPlacement placement;
    QString file;      // what to play: the transcoded copy when there is one, else the original
    QString original;  // the source file of the variant
    int bucket = 0;    // longest side of the transcoded copy in `file`; 0 when it is the original
    QSize native;      // size of the animation in pixels
    double speed = 1;  // mpv playback speed: placement.fps over the file's own average rate or placement.speed, 1 for a retimed copy
    bool arrange = false;
    bool paused = false; // placement.paused, or the session is locked or the screen is off, or the pets are hidden
    bool hidden = false; // the user hid all the pets (PetsHidden): the window is unmapped, its frames and its place are kept
    // The GPU frame cache. spriteSize is the pixel size the frames are wanted at (empty: the pet plays through the
    // video path only). The window decodes `file` at that size whenever spriteKey changes to something new, within
    // spriteBudget bytes, and plays the frames instead of the video once they are uploaded; it keeps showing the
    // frames it has until the new ones are ready. The controller changes the key only when a decode may start.
    QSize spriteSize;
    QString spriteKey;
    qint64 spriteBudget = 0;
    // The picture of the state likely to come next, decoded ahead into a second set of frames (only for a pet on the frame cache whose states have room in the
    // budget): preloadKey is to become spriteKey when the pet changes state, and the window then swaps the sets instead of decoding. Empty key: none (the set is dropped).
    // Another value than before: the animation on screen starts again from its first frame (a click on a `once` entry that is the one playing).
    int restartToken = 0;
    QString preloadFile;
    QSize preloadSize;
    QString preloadKey;
    qint64 preloadBudget = 0;
    // How the frames are timed (placement.fps and placement.speed): applied to the frames on screen without decoding
    // again. The video path gets the same through `speed` (and a retimed copy).
    PetTiming timing;
    int videoFpsCap = 0; // the same limit for the libmpv path, only when the animation is faster than it (0: none)
    int fpsCap = 0; // the most frames per second the pet may change at (0: no limit); frames above it are skipped
    // The pointer input (docs/pets/behaviours.md). The hit masks of a pet that plays from the frame cache are made by the window while it
    // decodes (maskBudget bytes at most); `masks` are those of a pet that plays through the video path, made by the controller's mask pass
    // of the file it plays (null until they arrive; the window then takes its whole rectangle). `pointer` is the session's level.
    std::shared_ptr<const MaskSet> masks;
    qint64 maskBudget = 0;
    PointerSource::Level pointer = PointerSource::Level::Local;
    // Rotation and flips are drawn (the window is the footprint): the masks are turned the same way. False until the window does so.
    bool transformApplied = false;
    // A puppet pet (docs/pets/puppets.md section 22): the picture comes from an engine process, not from `file`; there are no sprites, no transcode and no mask pass. The window starts and restarts the
    // engine and reports what it did (Callbacks::puppetCrashed, puppetRefused); the hit mask it gets from the engine becomes its input region.
    struct Puppet {
        bool active = false;
        QString dir;            // the item folder
        QString file;           // the model file in it, relative
        bool sandbox = true;
        bool requireSandbox = true;
        bool blocked = false;   // the model took the engine down three times: no engine
        bool look = false;      // the gaze follows the pointer (Mouse Tracking `look`)
        QString expression;     // the expression that shows ("" none)
        QString group;          // a motion group to play when playSerial changes ("" none)
        int playSerial = 0;
        int fps = 60;
        qint64 maxPixels = 4194304; // the setting puppetMaxPixels
        QStringList tapGroups;      // the motion groups a click falls back to when it is on no hit area (TapBody, Tap), those the model has, in that order
    };
    Puppet puppet;
};

// A window that shows one pet. An interface so the controller can be tested without a compositor.
class PetSurface
{
public:
    struct Callbacks {
        std::function<void(const QJsonObject &)> edited; // the user moved or resized it: {x, y, width}
        std::function<void()> removeRequested;           // the close button
        std::function<void(const QString &)> failed;     // the file could not be played
        std::function<void()> started;                   // the file plays (its first frame is out)
        // The decode of the last spriteKey ended: the frames are on screen (ok, bytes on the GPU) or could not be
        // made (the window plays the video).
        std::function<void(bool ok, qint64 bytes, const QString &reason)> spriteDone;
        // The decode of preloadKey ended: ready (ok, bytes the second set takes) or not made.
        std::function<void(bool ok, qint64 bytes, const QString &key, const QString &reason)> preloadDone;
        // Whether the pet needs the global pointer (hide, fade or dodge on, at the global level) changed.
        std::function<void()> pointerWantChanged;
        // The animation wraps to its first frame at `monotonicNs` on CLOCK_MONOTONIC (the first showing is loop 0): announced a little ahead of the time, so
        // that the sound of a pet in the mode animation_loop can be scheduled to it exactly. Only a pet that plays from the frame cache reports it.
        std::function<void(qint64 monotonicNs, qint64 loop)> wrapped;
        // The announced wrap will not happen then (pause, new timing, new frames); a new announcement follows.
        std::function<void()> wrapCancelled;
        // The pointer was pressed and released on the pet without a drag (the sound mode click).
        std::function<void()> clicked;
        // What the pointer did to the pet (a drag began, followed the pointer, ended; a dodge step), for the states of the pet (docs/pets/states.md). The controller
        // makes the pet's picture follow; a window whose pet has no states may still send them.
        std::function<void(const PetPointerEvent &)> pointerEvent;
        // The engine of a puppet pet died of something that is not a refusal (afterLongRun: it had been drawing for kEngineLongRunSeconds), or will not be started again (a name of puppetnames.h or a short reason).
        std::function<void(bool afterLongRun)> puppetCrashed;
        std::function<void(const QString &name)> puppetRefused;
    };

    virtual ~PetSurface() = default;

    // Shows the view. Called again for every change; a surface that is being edited by the user keeps its own
    // geometry until the edit has been committed.
    virtual void apply(const PetView &view) = 0;
    // Whether the surface takes pointer input (the arrange mode).
    virtual bool inputEnabled() const = 0;
    // Position and size in output-logical pixels, as shown.
    virtual QRectF geometry() const = 0;

    // The global cursor in the compositor's logical coordinates (PointerSource::moved); a surface that wants it (wantsGlobalPointer) maps it
    // to its output. Surfaces that do not take pointer input ignore it.
    virtual void globalPointer(QPointF position) { Q_UNUSED(position); }
    virtual bool wantsGlobalPointer() const { return false; }
    // What Windows() reports about the pointer input: level, hit mask, bytes, hidden by hover, the names of what degrades.
    virtual PetInputStatus inputStatus() const { return {}; }

    // The position, in seconds, of the video this window plays (-1 when it has none: not loaded, or paused), and a seek of it. The windows of a `desktop` stage are
    // kept in step by them (spansync.h).
    virtual double videoPosition() const { return -1; }
    // The length of that video in seconds (-1: not known); the windows of a loop are compared on the circle, not on the line.
    virtual double videoDuration() const { return -1; }
    // How long the last seekVideo took to land, in seconds (-1: none yet): the video went on playing meanwhile, so a seek aims that far ahead.
    virtual double videoSeekLatency() const { return -1; }
    virtual void seekVideo(double seconds) { Q_UNUSED(seconds); }

    // Test hook: does what the pointer would. kind is "move" (a, b: dx, dy), "scale" (a: factor, about the top-left
    // corner) or "resize" (a: delta of the width). False when the kind is unknown.
    // The names of what the surface does less than asked (docs/pets/behaviours.md, "Names"); empty when it does what was asked.
    virtual QStringList degraded() const { return {}; }

    virtual bool simulateEdit(const QString &kind, double a, double b)
    {
        Q_UNUSED(kind);
        Q_UNUSED(a);
        Q_UNUSED(b);
        return false;
    }

    void setCallbacks(Callbacks callbacks) { m_callbacks = std::move(callbacks); }

protected:
    Callbacks m_callbacks;
};

} // namespace murasaki
