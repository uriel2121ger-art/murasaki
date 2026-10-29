#pragma once

#include "petgeometry.h"
#include "petinput.h"
#include "petsurface.h"

#include <QPointer>
#include <QQuickView>
#include <QElapsedTimer>
#include <QTimer>

class QMouseEvent;

class QQmlEngine;
namespace murasaki {
class SpriteItem;
class PuppetItem;
}
class QScreen;

namespace LayerShellQt {
class Window;
}

namespace murasaki {

// The window of one pet: a layer-shell surface on the overlay layer, transparent and above everything.
//
// Normally it is as large as the pet and anchored to the top and left edge of its output, so that the margins are
// the pet's position, and it lets the pointer through (Qt::WindowTransparentForInput).
//
// In the arrange mode it covers its whole output instead, with the pet drawn at its position inside it and the
// input region cut down to the pet's rectangle (QWindow::setMask). The pointer position is then the position on
// the output, and the window never moves while it is edited, which a pet-sized window that follows the pointer
// can not offer (its pointer coordinates move with it, a frame behind). Dragging moves the pet inside the window,
// the wheel and the corner handle scale it, the cross removes it; an edit is sent to the daemon when the button
// is released (the wheel: shortly after the last notch).
class PetWindow : public QQuickView, public PetSurface
{
    Q_OBJECT

public:
    PetWindow(QScreen *screen, QQmlEngine *engine, const QString &petId);

    void apply(const PetView &view) override;
    // The arrange mode (the meaning of Windows()'s `interactive`); a pet that takes the pointer on its opaque pixels is not "interactive" in that sense.
    bool inputEnabled() const override { return m_arrange && !(flags() & Qt::WindowTransparentForInput); }
    QRectF geometry() const override;
    bool simulateEdit(const QString &kind, double a, double b) override;
    QStringList degraded() const override;
    void globalPointer(QPointF position) override;
    double videoPosition() const override;
    double videoDuration() const override;
    double videoSeekLatency() const override;
    void seekVideo(double seconds) override;
    bool wantsGlobalPointer() const override { return m_input.wantsGlobalPointer() || m_puppetLook; }
    PetInputStatus inputStatus() const override { return m_input.status(); }

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void onDragStarted(double x, double y);
    void onDragMoved(double x, double y);
    void onResizeStarted(double x, double y);
    void onResizeMoved(double x, double y);
    void onScaleAt(double factor, double x, double y);
    void endEdit();
    void onCloseRequested();
    void commit();
    void onPlaybackFailed(const QString &reason);
    void onPlaybackStarted();
    void onSpriteDone(bool ok, double bytes, const QString &reason);
    void onPreloadDone(bool ok, double bytes, const QString &key, const QString &reason);
    void onFrameChanged(int frame);
    void onWrapped(qint64 monotonicNs, qint64 loop);
    void onWrapCancelled();
    void onQmlWrapped(double monotonicNs, double loop);
    void onClicked();
    void onPuppetHitMask(int cols, int rows, int cell, const QByteArray &bits);
    void onInputChanged();
    void onInputMove(double dx, double dy);

private:
    void applyLayout();
    void applyKeepAbove(bool keepAbove);
    void scheduleCommit();
    void setWidthClamped(double width);
    void clampToOutput();

    // The pointer input (PetInput): what the window takes from it and gives to it. Kept apart from the layout code above.
    void applyInput(const PetView &view);     // the view's pointer settings
    void applyInputRegion(QPoint offset);     // the input region from PetInput, shifted by the pet's place when the window covers the output
    void applyOpacity();                      // the placement's opacity times PetInput's factor
    void syncInputGeometry();                 // the window rectangle and the output, for PetInput
    void clickPuppet();
    void applyPuppet(const PetView &view);   // the puppet pet's engine item: its properties, what it reports and the commands it is given
    void updateInputMasks();                  // the masks of the frames on screen, else those of the controller's pass
    SpriteItem *activeSprite() const;         // the set of frames on screen (the other one holds the preloaded picture)
    bool inputActive() const;                 // events go to PetInput (not the arrange mode)

    QString m_petId;
    LayerShellQt::Window *m_layer = nullptr;
    QString m_outputName;     // the output the window was made for (the title in shell mode names it)
    bool m_titleBelow = false; // the title says "below" (keepAbove off), for the GNOME extension
    bool m_shell = false; // GNOME: a plain window pinned by the shell extension, see shellwindows.h
    QSize m_native{1, 1};
    double m_x = 0;
    double m_y = 0;
    double m_width = 256;
    bool m_shown = false;
    bool m_releaseUnmapped = false; // unmapped for a click that goes through the faded pet (PetInput::released)
    bool m_viewHidden = false;      // the last view's `hidden` (by the user)
    bool m_keepAbove = true;
    bool m_keepAboveApplied = false; // applyKeepAbove has run once
    double m_rotation = 0;           // degrees, 0 to 360, as stored
    bool m_flipH = false;
    bool m_flipV = false;
    bool m_x11 = false; // an X11 session: no layer-shell, an override-redirect window
    QTimer m_raiseTimer;
    bool m_arrange = false;
    bool m_expanded = false; // the layout currently applied
    bool m_stageHidden = false; // hidden or paused: it has no position to compare
    bool m_stage = false;    // the fullscreen mode: the window is its output and the animation is drawn in m_stageRect (docs/pets/stage.md)
    QRectF m_stageRect;
    // Editing: the user's geometry wins over the daemon's until the edit is committed and has come back.
    bool m_editing = false;
    QDeadlineTimer m_holdUntil; // expired until the first commit
    QTimer m_commitTimer;
    double m_grabX = 0; // where in the pet the pointer took hold of it
    double m_grabY = 0;
    double m_resizeStartWidth = 0;
    PetRect m_resizeStartRect;   // the pet (unrotated rectangle) when the corner handle was taken
    double m_resizeStartX = 0;
    double m_resizeStartY = 0;
    // The pointer input
    PetInput m_input;
    double m_basePetOpacity = 1;
    std::shared_ptr<const MaskSet> m_viewMasks;
    // A puppet pet: the footprint its engine sent last (the input region), whether its gaze follows the pointer, and the motion request it has played.
    std::shared_ptr<const MaskSet> m_puppetMasks;
    bool m_puppet = false;
    bool m_puppetLook = false;
    int m_puppetSerial = 0;
    QPointer<PuppetItem> m_puppetItem;
    QStringList m_puppetTapGroups;
    QPointF m_lastPress;     // where the last press was, in the window (a click on a puppet is judged there)
    bool m_wantedGlobal = false;
    bool m_dragMoved = false;          // the pet moved during this press (a click that does not move it sends nothing)
    QElapsedTimer m_regionClock;       // when the input region was last handed to the compositor
    QTimer m_regionDeferred;           // a region change that only moved it, held back during a drag (at most 30 a second)
    bool m_appliedKnown = false;       // m_applied* describe what the window has; false after the arrange mode set its own
    bool m_appliedHaveRegion = false;
    std::vector<MaskRect> m_appliedRects;
    QPoint m_appliedOffset;
};

} // namespace murasaki
