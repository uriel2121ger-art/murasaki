#include "petwindow.h"

#include "puppetitem.h"
#ifdef MURASAKI_HAVE_RELATIVE_POINTER
#include "relativepointer.h"
#endif
#include "puppetmask.h"
#include "puppetnames.h"

#include "petbehaviours.h"
#include "petgeometry.h"
#include "petstage.h"
#include "shellwindows.h"
#include "spriteitem.h"

#include <QEnterEvent>
#include <QMouseEvent>
#include <QRegion>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScreen>
#include <QSurfaceFormat>

#include <LayerShellQt/window.h>

#ifdef MURASAKI_HAVE_X11
#include "x11hints.h"
#endif

#include <QGuiApplication>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int kCommitDelayMs = 350;   // after the last pointer event
constexpr int kHoldAfterCommitMs = 1500; // the daemon's answer to our own edit is not an outside change
constexpr int kRaiseMs = 2000;           // X11: how often a pet is stacked above the windows raised since

} // namespace

PetWindow::PetWindow(QScreen *screen, QQmlEngine *engine, const QString &petId)
    : QQuickView(engine, nullptr)
    , m_petId(petId)
{
    QSurfaceFormat format = this->format();
    format.setAlphaBufferSize(8);
    setFormat(format);
    setColor(Qt::transparent);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setScreen(screen);
    setFlags(petWindowFlags(false));
    connect(this, &QWindow::screenChanged, this, [this] { applyLayout(); });
#ifdef MURASAKI_HAVE_RELATIVE_POINTER
    // (every pet hears it; only the one being dragged reads it)
    if (auto *relative = murasaki::RelativePointer::instance())
        connect(relative, &murasaki::RelativePointer::moved, this, [this](double dx, double dy) {
            if (inputActive())
                m_input.relativeMotion(dx, dy);
        });
#endif

#ifdef MURASAKI_HAVE_X11
    m_x11 = QGuiApplication::platformName().startsWith(u"xcb"_s);
    if (m_x11) {
        // An X11 pet is an override-redirect window (no window manager moves or decorates it) above everything, whose
        // position is the output's origin plus the pet's own. Real translucency needs a compositing manager.
        setFlags(petX11WindowFlags(false));
        murasaki::x11::applyPetHints(this);
        // Something raised later covers an override-redirect window: put the pet back on top now and then.
        m_raiseTimer.setInterval(kRaiseMs);
        connect(&m_raiseTimer, &QTimer::timeout, this, [this] { murasaki::x11::raiseWindow(this); });
    }
#endif

    // GNOME on Wayland: plain windows pinned by the shell extension. On X11 (even in a GNOME session) the X11 presenter wins.
    m_shell = useShellWindows() && !m_x11;
    if (m_shell) {
        // GNOME: no layer-shell. The window covers its output (like the arrange mode) and the extension of the shell pins
        // it above everything, finding it by the title (docs/gnome.md).
        m_outputName = screen ? screen->name() : QString();
        setTitle(shellPetTitle(m_outputName, petId));
    } else if (auto *layer = m_x11 ? nullptr : LayerShellQt::Window::get(this)) {
        using LayerWindow = LayerShellQt::Window;
        m_layer = layer;
        layer->setLayer(LayerWindow::LayerOverlay);
        layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop | LayerWindow::AnchorLeft));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerWindow::KeyboardInteractivityNone);
        layer->setScope(u"murasaki-pet"_s);
#if MURASAKI_LAYERSHELL_HAS_SCREEN
        layer->setScreen(screen);
#endif
    }

    setSource(QUrl(u"qrc:/qt/qml/Murasaki/Pets/PetItem.qml"_s));
    for (const QQmlError &error : errors())
        qWarning("murasaki-overlay: %s", qPrintable(error.toString()));
    if (QQuickItem *root = rootObject()) {
        connect(root, SIGNAL(dragStarted(double, double)), this, SLOT(onDragStarted(double, double)));
        connect(root, SIGNAL(dragMoved(double, double)), this, SLOT(onDragMoved(double, double)));
        connect(root, SIGNAL(editEnded()), this, SLOT(endEdit()));
        connect(root, SIGNAL(resizeStarted(double, double)), this, SLOT(onResizeStarted(double, double)));
        connect(root, SIGNAL(resizeMoved(double, double)), this, SLOT(onResizeMoved(double, double)));
        connect(root, SIGNAL(scaleAt(double, double, double)), this, SLOT(onScaleAt(double, double, double)));
        connect(root, SIGNAL(closeRequested()), this, SLOT(onCloseRequested()));
        connect(root, SIGNAL(playbackFailed(QString)), this, SLOT(onPlaybackFailed(QString)));
        connect(root, SIGNAL(playbackStarted()), this, SLOT(onPlaybackStarted()));
        connect(root, SIGNAL(spriteDone(bool, double, QString)), this, SLOT(onSpriteDone(bool, double, QString)));
        connect(root, SIGNAL(frameChanged(int)), this, SLOT(onFrameChanged(int)));
        connect(root, SIGNAL(wrapped(double, double)), this, SLOT(onQmlWrapped(double, double)));
        connect(root, SIGNAL(wrapCancelled()), this, SLOT(onWrapCancelled()));
        connect(root, SIGNAL(preloadDone(bool, double, QString, QString)), this, SLOT(onPreloadDone(bool, double, QString, QString)));
        // Two sets of frames (the one on screen and the preloaded one): only the one on screen speaks for the pet.
        const auto sprites = root->findChildren<SpriteItem *>(QString(), Qt::FindChildrenRecursively);
        for (SpriteItem *sprite : sprites) {
            connect(sprite, &SpriteItem::wrapped, this, [this, sprite](qint64 monotonicNs, qint64 loop) {
                if (sprite == activeSprite())
                    onWrapped(monotonicNs, loop);
            });
            connect(sprite, &SpriteItem::wrapCancelled, this, [this, sprite] {
                if (sprite == activeSprite())
                    onWrapCancelled();
            });
        }
    }
    connect(&m_input, &PetInput::clicked, this, &PetWindow::onClicked);
    connect(&m_input, &PetInput::changed, this, &PetWindow::onInputChanged);
    connect(&m_input, &PetInput::moveRequested, this, &PetWindow::onInputMove);
    connect(&m_input, &PetInput::pointerEvent, this, [this](const PetPointerEvent &event) {
        if (m_callbacks.pointerEvent)
            m_callbacks.pointerEvent(event);
    });
    m_commitTimer.setSingleShot(true);
    m_commitTimer.setInterval(kCommitDelayMs);
    connect(&m_commitTimer, &QTimer::timeout, this, &PetWindow::commit);
    m_regionDeferred.setSingleShot(true);
    connect(&m_regionDeferred, &QTimer::timeout, this, &PetWindow::applyLayout);
}

// The libmpv item of the window (the one the Loader of PetItem.qml makes while the frames are not on the GPU), found by its class.
static QQuickItem *videoItemOf(QQuickItem *root)
{
    if (!root)
        return nullptr;
    const auto items = root->findChildren<QQuickItem *>(QString(), Qt::FindChildrenRecursively);
    for (QQuickItem *item : items) {
        if (QByteArrayView(item->metaObject()->className()).contains("VideoItem"))
            return item;
    }
    return nullptr;
}

double PetWindow::videoPosition() const
{
    QQuickItem *video = videoItemOf(rootObject());
    if (!video || m_stageHidden)
        return -1;
    double position = -1;
    QMetaObject::invokeMethod(video, "position", Q_RETURN_ARG(double, position));
    return position;
}

double PetWindow::videoDuration() const
{
    QQuickItem *video = videoItemOf(rootObject());
    if (!video)
        return -1;
    double duration = -1;
    QMetaObject::invokeMethod(video, "duration", Q_RETURN_ARG(double, duration));
    return duration;
}

double PetWindow::videoSeekLatency() const
{
    QQuickItem *video = videoItemOf(rootObject());
    if (!video)
        return -1;
    double latency = -1;
    QMetaObject::invokeMethod(video, "seekLatency", Q_RETURN_ARG(double, latency));
    return latency;
}

void PetWindow::seekVideo(double seconds)
{
    if (QQuickItem *video = videoItemOf(rootObject()))
        QMetaObject::invokeMethod(video, "seekTo", Q_ARG(double, seconds));
}

QRectF PetWindow::geometry() const
{
    if (m_stage)
        return m_stageRect;
    const QSize size = petDisplaySize(m_width, m_native);
    return QRectF(m_x, m_y, size.width(), size.height());
}

void PetWindow::apply(const PetView &given)
{
    PetView view = given;
    m_stage = view.stage.active;
    m_stageHidden = view.hidden || view.paused;
    m_stageRect = view.stage.petRect;
    if (m_stage) {
        // A stage takes no pointer, whatever else the placement says (docs/pets/stage.md): it is click-through, not draggable, not arranged, and
        // hide, fade and dodge are stored with the pet but do not apply.
        view.arrange = false;
        view.placement.clickThrough = true;
        view.placement.locked = true;
        view.placement.hoverHide = false;
        view.placement.focusFade = false;
        view.placement.smartDodge = false;
    }
    m_native = view.native.isValid() ? view.native : QSize(1, 1);
    m_rotation = normalizeRotation(view.placement.rotation);
    m_flipH = view.placement.flipH;
    m_flipV = view.placement.flipV;
    if (!m_editing && !m_commitTimer.isActive() && m_holdUntil.hasExpired()) {
        m_x = view.placement.x;
        m_y = view.placement.y;
        m_width = view.placement.width;
    }
    if (m_stage && m_arrange) {
        m_arrange = false; // (an edit in progress ends; nothing of a stage is edited)
        m_editing = false;
    }
    if (view.arrange != m_arrange || !m_shown) {
        m_arrange = view.arrange;
        if (!m_arrange) {
            // Leaving the mode ends any edit; an unsent one goes out now.
            if (m_commitTimer.isActive())
                commit();
            m_editing = false;
        }
    }
    m_viewHidden = view.hidden;
    applyInput(view); // before the layout: whether the window grows for a drag, and what its input region is, come from it
    applyLayout();
    QQuickItem *root = rootObject();
    if (root && view.hidden) {
        // Hidden: only the pause. A new file or a new size (a resize, another screen) waits for the pets to be shown, so that nothing is
        // decoded or opened for a window nobody sees; the next apply() after the show sets everything.
        root->setProperty("paused", true);
    } else if (root) {
        root->setProperty("source", view.file);
        root->setProperty("paused", view.paused);
        root->setProperty("arrange", m_arrange);
        m_basePetOpacity = view.placement.opacity;
        applyOpacity();
        root->setProperty("speed", view.speed);
        // The sprite's properties go in this order: the key is what starts a decode, of the source and size above.
        root->setProperty("spriteEnabled", view.spriteSize.isValid());
        root->setProperty("spriteW", view.spriteSize.width());
        root->setProperty("spriteH", view.spriteSize.height());
        root->setProperty("spriteBudget", double(view.spriteBudget));
        root->setProperty("maskBudget", double(view.maskBudget));
        root->setProperty("timingFps", view.timing.fps);
        root->setProperty("timingSpeed", view.timing.speed);
        root->setProperty("fpsCap", view.fpsCap);
        root->setProperty("videoFpsCap", view.videoFpsCap);
        root->setProperty("spriteKey", view.spriteKey);
        root->setProperty("restartToken", view.restartToken);
        // The preloaded picture goes after the key: a change of state hands over a key that was preloaded (the sets swap) and the next preload in one apply.
        root->setProperty("preloadSource", view.preloadFile);
        root->setProperty("preloadW", view.preloadSize.width());
        root->setProperty("preloadH", view.preloadSize.height());
        root->setProperty("preloadBudget", double(view.preloadBudget));
        root->setProperty("preloadKey", view.preloadKey);
        root->setProperty("petRotation", m_rotation);
        root->setProperty("flipH", m_flipH);
        root->setProperty("flipV", m_flipV);
        applyPuppet(view);
    }
    applyKeepAbove(view.placement.keepAbove);
    if (m_shell) {
        // GNOME: keepAbove and the fullscreen mode are told to the extension in the title, which arranges the window again when it changes.
        const QString stage = view.stage.active ? QString::fromLatin1(stageModeName(view.placement.stage.mode)) : QString();
        const QString wanted = shellPetTitle(m_outputName, m_petId, !view.placement.keepAbove, stage);
        if (wanted != title()) {
            m_titleBelow = !view.placement.keepAbove;
            setTitle(wanted);
        }
    }
    if (view.hidden) {
        // Hidden by the user: unmapped, with everything kept (frames, place, state); the controller has paused it.
        if (isVisible())
            hide();
        m_raiseTimer.stop();
    } else if (!m_shown || (!isVisible() && !m_releaseUnmapped)) {
        m_shown = true;
        show();
        if (m_x11) {
            if (m_keepAbove)
                m_raiseTimer.start();
            else
                murasaki::x11::lowerPet(this); // (mapping put it on top of the stack)
        }
    }
}

// keepAbove off. Layer-shell: the bottom layer (above the wallpaper and the desktop's icons, below every window; a layer surface is never
// "a normal window", and setLayer on a mapped surface does not unmap it on KWin: docs/pets/pointer-facts.md, U4). X11: exact, the pet loses the
// ABOVE state and the stay-on-top hint and is no longer raised, so it is stacked like an ordinary window. GNOME: the title says "below" and the
// extension decides (experimental). The names of what is less than asked are in degraded().
void PetWindow::applyKeepAbove(bool keepAbove)
{
    if (keepAbove == m_keepAbove && m_keepAboveApplied)
        return;
    m_keepAbove = keepAbove;
    m_keepAboveApplied = true;
    if (m_layer)
        m_layer->setLayer(keepAbove ? LayerShellQt::Window::LayerOverlay : LayerShellQt::Window::LayerBottom);
#ifdef MURASAKI_HAVE_X11
    if (m_x11) {
        setFlag(Qt::WindowStaysOnTopHint, keepAbove);
        murasaki::x11::setPetKeepAbove(this, keepAbove);
        if (keepAbove) {
            if (isVisible())
                m_raiseTimer.start();
        } else {
            m_raiseTimer.stop();
            // An override-redirect window stays where it is in the stack when the hints go: really put it below the ordinary windows.
            murasaki::x11::lowerPet(this);
        }
    }
#endif
}

QStringList PetWindow::degraded() const
{
    QStringList names;
    // (a layer-shell window that never got its layer, because the protocol was missing, did nothing to degrade)
    if (!m_shell && !m_x11 && !m_layer)
        return names;
    if (const auto name = degrade::keepAboveDegrade(m_keepAbove, m_shell ? degrade::PetPresenter::Shell : m_x11 ? degrade::PetPresenter::X11 : degrade::PetPresenter::LayerShell); !name.empty())
        names << QString::fromUtf8(name.data(), qsizetype(name.size()));
    if (m_puppetItem && m_puppetItem->resolutionReduced())
        names << QString::fromUtf8(puppet::kResolutionReduced.data(), qsizetype(puppet::kResolutionReduced.size()));
    if (m_stage && m_shell && !m_keepAbove)
        names << QString::fromUtf8(stagedegrade::kBelowUnsupported.data(), qsizetype(stagedegrade::kBelowUnsupported.size()));
    return names;
}

void PetWindow::applyLayout()
{
    const QSize size = petDisplaySize(m_width, m_native);
    // The window of a rotated pet is the footprint of its rectangle (the bounding box of the turned one), centred on the pet's own centre; x, y and
    // the width keep meaning the unrotated pet's. Rotation and flip are transforms of the scene graph (PetItem.qml): nothing is decoded again.
    const PetRect footprint = petWindowRect({m_x, m_y, double(size.width()), double(size.height())}, m_rotation);
    const QSize windowSize(petCeilPx(footprint.w), petCeilPx(footprint.h));
    const QRect footprintPx(QPoint(int(std::lround(footprint.x)), int(std::lround(footprint.y))), windowSize);
    QQuickItem *root = rootObject();
    if (m_shell) {
        // Always output-sized, the pet drawn at its position inside; the input region is the pet's rectangle while
        // arranging and empty (click-through) otherwise.
        const QSize output = screen() ? screen()->size() : size;
        m_expanded = true;
        if (this->size() != output)
            resize(output);
        if (root) {
            root->setProperty("petX", m_stage ? m_stageRect.x() : m_x);
            root->setProperty("petY", m_stage ? m_stageRect.y() : m_y);
            root->setProperty("petW", m_stage ? m_stageRect.width() : double(size.width()));
            root->setProperty("petH", m_stage ? m_stageRect.height() : double(size.height()));
            root->setProperty("expanded", true);
        }
        if (m_arrange) {
            setMask(QRegion(footprintPx));
            setFlag(Qt::WindowTransparentForInput, false);
            m_appliedKnown = false; // (the region cache does not know what the arrange mode set)
        } else {
            applyInputRegion(footprintPx.topLeft());
        }
        syncInputGeometry();
        return;
    }
    // The window covers the output while arranging, and while a pet is dragged (pointer positions are then output positions, and the
    // window does not move under the pointer).
    if (m_stage) {
        // A stage: the window is its output (all four anchors, no margins), the animation is drawn in its rectangle, and nothing takes the pointer. It
        // is a layer surface (or the X11 window with the pet hints), never a fullscreen window: no monitor takes it for a fullscreen application.
        using LayerWindow = LayerShellQt::Window;
        const QSize output = screen() ? screen()->size() : size;
        if (!m_expanded && m_layer) {
            m_layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop | LayerWindow::AnchorBottom
                                                     | LayerWindow::AnchorLeft | LayerWindow::AnchorRight));
            m_layer->setMargins(QMargins());
        }
        m_expanded = true;
        if (m_x11 && screen())
            setPosition(screen()->geometry().topLeft());
        if (this->size() != output)
            resize(output);
        if (root) {
            root->setProperty("petX", m_stageRect.x());
            root->setProperty("petY", m_stageRect.y());
            root->setProperty("petW", m_stageRect.width());
            root->setProperty("petH", m_stageRect.height());
            root->setProperty("expanded", true);
        }
        applyInputRegion(QPoint(0, 0));
        syncInputGeometry();
        return;
    }
    const bool grown = m_arrange || m_input.layout() == PetLayout::Drag;
    if (grown) {
        using LayerWindow = LayerShellQt::Window;
        const QSize output = screen() ? screen()->size() : size;
        if (!m_expanded && m_layer) {
            m_layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop | LayerWindow::AnchorBottom
                                                     | LayerWindow::AnchorLeft | LayerWindow::AnchorRight));
            m_layer->setMargins(QMargins());
        }
        m_expanded = true;
        if (m_x11 && screen())
            setPosition(screen()->geometry().topLeft());
        if (this->size() != output)
            resize(output);
        if (root) {
            root->setProperty("petX", m_x);
            root->setProperty("petY", m_y);
            root->setProperty("petW", double(size.width()));
            root->setProperty("petH", double(size.height()));
            root->setProperty("expanded", true);
        }
        // Only the pet takes the pointer; the rest of the output keeps working underneath.
        if (m_arrange) {
            setMask(QRegion(footprintPx));
            setFlag(Qt::WindowTransparentForInput, false);
            m_appliedKnown = false;
        } else {
            applyInputRegion(footprintPx.topLeft()); // (a drag) the mask of the turned pet, where the footprint is
        }
    } else {
        if (m_expanded && m_layer) {
            using LayerWindow = LayerShellQt::Window;
            m_layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop | LayerWindow::AnchorLeft));
        }
        m_expanded = false;
        if (root) {
            root->setProperty("petW", double(size.width()));
            root->setProperty("petH", double(size.height()));
            root->setProperty("expanded", false);
        }
        applyInputRegion(QPoint(0, 0));
        if (this->size() != windowSize)
            resize(windowSize);
        if (m_layer)
            m_layer->setMargins(QMargins(footprintPx.x(), footprintPx.y(), 0, 0));
        if (m_x11 && screen())
            setPosition(screen()->geometry().topLeft() + footprintPx.topLeft());
    }
    syncInputGeometry();
}

void PetWindow::setWidthClamped(double width)
{
    m_width = clampPetWidth(width, m_native.width());
}

// A pet dragged off the output would be lost: keep a part of it where the pointer can still take it.
void PetWindow::clampToOutput()
{
    if (!screen())
        return;
    // The rule is the footprint's: at least 48 px of the turned pet stay where the pointer can take them.
    const QSize size = petDisplaySize(m_width, m_native);
    const PetRect footprint = petWindowRect({m_x, m_y, double(size.width()), double(size.height())}, m_rotation);
    const QSize output = screen()->size();
    const PetRect kept = murasaki::clampToOutput(footprint, {0, 0, double(output.width()), double(output.height())});
    m_x += kept.x - footprint.x;
    m_y += kept.y - footprint.y;
}

void PetWindow::scheduleCommit()
{
    m_editing = true;
    m_commitTimer.start();
}

void PetWindow::onDragStarted(double x, double y)
{
    m_grabX = x - m_x;
    m_grabY = y - m_y;
    m_editing = true;
    m_commitTimer.stop();
}

void PetWindow::onDragMoved(double x, double y)
{
    m_x = x - m_grabX;
    m_y = y - m_grabY;
    applyLayout();
}

void PetWindow::onResizeStarted(double x, double y)
{
    m_resizeStartWidth = m_width;
    const QSize startSize = petDisplaySize(m_width, m_native);
    m_resizeStartRect = {m_x, m_y, double(startSize.width()), double(startSize.height())};
    m_resizeStartX = x;
    m_resizeStartY = y;
    m_editing = true;
    m_commitTimer.stop();
}

void PetWindow::onResizeMoved(double x, double y)
{
    const double aspect = double(m_native.width()) / std::max(1, m_native.height());
    // The pointer moves on the output; the corner moves along the turned pet's own axes (a flip mirrors them, which the sign of the diagonal covers).
    const PetPoint turned = rotatePoint({x - m_resizeStartX, y - m_resizeStartY}, {0, 0}, -m_rotation);
    const double dx = turned.x;
    const double dy = turned.y * aspect;
    setWidthClamped(m_resizeStartWidth + (std::abs(dx) >= std::abs(dy) ? dx : dy));
    // The corner opposite to the handle stays where it was on the screen, whatever the angle (growing about the unrotated top-left would move it).
    const QSize size = petDisplaySize(m_width, m_native);
    const PetPoint topLeft = petTopLeftKeepingTopLeftCorner(m_resizeStartRect, {double(size.width()), double(size.height())}, m_rotation);
    m_x = topLeft.x;
    m_y = topLeft.y;
    applyLayout();
}

void PetWindow::onScaleAt(double factor, double x, double y)
{
    const double before = m_width;
    setWidthClamped(m_width * factor);
    const double ratio = m_width / before;
    // Keep the point under the pointer where it is.
    m_x += (x - m_x) * (1 - ratio);
    m_y += (y - m_y) * (1 - ratio);
    applyLayout();
    scheduleCommit();
}

void PetWindow::endEdit()
{
    m_commitTimer.stop();
    commit();
}

void PetWindow::onCloseRequested()
{
    if (m_callbacks.removeRequested)
        m_callbacks.removeRequested();
}

void PetWindow::commit()
{
    m_commitTimer.stop();
    m_editing = false;
    clampToOutput();
    applyLayout();
    m_holdUntil = QDeadlineTimer(kHoldAfterCommitMs);
    if (m_callbacks.edited) {
        m_callbacks.edited(QJsonObject{{u"x"_s, std::round(m_x)},
                                       {u"y"_s, std::round(m_y)},
                                       {u"width"_s, std::round(m_width)}});
    }
}

bool PetWindow::simulateEdit(const QString &kind, double a, double b)
{
    if (kind == u"move"_s) {
        m_x += a;
        m_y += b;
    } else if (kind == u"scale"_s) {
        setWidthClamped(m_width * a);
    } else if (kind == u"resize"_s) {
        setWidthClamped(m_width + a);
    } else {
        return false;
    }
    applyLayout();
    scheduleCommit();
    return true;
}

void PetWindow::onPlaybackFailed(const QString &reason)
{
    if (m_callbacks.failed)
        m_callbacks.failed(reason);
}

SpriteItem *PetWindow::activeSprite() const
{
    QQuickItem *root = rootObject();
    return root ? qobject_cast<SpriteItem *>(root->property("frontSprite").value<QObject *>()) : nullptr;
}

void PetWindow::onPreloadDone(bool ok, double bytes, const QString &key, const QString &reason)
{
    if (m_callbacks.preloadDone)
        m_callbacks.preloadDone(ok, qint64(bytes), key, reason);
}

void PetWindow::onSpriteDone(bool ok, double bytes, const QString &reason)
{
    updateInputMasks(); // the new frames bring their masks (or, failing, the video path takes over)
    if (m_callbacks.spriteDone)
        m_callbacks.spriteDone(ok, qint64(bytes), reason);
}

void PetWindow::onPlaybackStarted()
{
    if (m_callbacks.started)
        m_callbacks.started();
}

// ---- the pointer input ---------------------------------------------------------------------------------------------------------
// (PetInput does the thinking: PetInteraction, the masks, the region; these functions only carry things across.)

namespace {
// MURASAKI_DEBUG_INPUT=1 logs what the pointer input does (regions applied, events received), for a compositor that surprises.
bool debugInput()
{
    static const bool on = qEnvironmentVariableIntValue("MURASAKI_DEBUG_INPUT") == 1;
    return on;
}
} // namespace

bool PetWindow::inputActive() const
{
    return !m_arrange && m_input.enabled();
}

void PetWindow::applyInput(const PetView &view)
{
    m_input.setEnabled(!m_arrange && !view.hidden);
    m_input.setLevel(view.pointer);
    PetBehaviourConfig config;
    config.hoverHide = view.placement.hoverHide;
    config.focusFade = view.placement.focusFade && !view.placement.hoverHide;
    config.smartDodge = view.placement.smartDodge;
    config.clickThrough = view.placement.clickThrough;
    config.locked = view.placement.locked;
    m_input.setBehaviour(config);
    // The masks turn the way the picture is drawn (PetItem.qml: flips in the picture's frame, then the rotation about its centre).
    if (view.transformApplied)
        m_input.setTransform(m_rotation, view.placement.flipH, view.placement.flipV);
    else
        m_input.setTransform(0, false, false);
    m_viewMasks = view.puppet.active ? m_puppetMasks : view.masks;
    updateInputMasks();
    const bool wants = m_input.wantsGlobalPointer();
    if (wants != m_wantedGlobal) {
        m_wantedGlobal = wants;
        if (m_callbacks.pointerWantChanged)
            m_callbacks.pointerWantChanged();
    }
}

// The frames on screen bring their own masks (indexed like the frames); a pet on the video path takes the controller's, of which only the
// union is used (which frame mpv shows is not known).
void PetWindow::updateInputMasks()
{
    SpriteItem *sprite = activeSprite();
    if (sprite && sprite->hasFrames() && sprite->masks()) {
        m_input.setMasks(sprite->masks(), true);
        m_input.setFrame(sprite->currentFrame());
    } else {
        m_input.setMasks(m_viewMasks, false);
    }
}

// A puppet pet: the engine's picture instead of the sprite and the video. The root's properties start (and restart, and stop) the PuppetItem of PetItem.qml; what it reports and the commands it takes go through the object.
void PetWindow::applyPuppet(const PetView &view)
{
    QQuickItem *root = rootObject();
    if (!root)
        return;
    m_puppet = view.puppet.active;
    m_puppetLook = view.puppet.active && view.puppet.look;
    m_puppetTapGroups = view.puppet.tapGroups;
    root->setProperty("puppetMode", view.puppet.active);
    if (!view.puppet.active) {
        m_puppetMasks.reset();
        return;
    }
    root->setProperty("puppetDir", view.puppet.dir);
    root->setProperty("puppetFile", view.puppet.file);
    root->setProperty("puppetSandbox", view.puppet.sandbox);
    root->setProperty("puppetRequireSandbox", view.puppet.requireSandbox);
    root->setProperty("puppetBlocked", view.puppet.blocked);
    root->setProperty("puppetLook", view.puppet.look);
    root->setProperty("puppetExpression", view.puppet.expression);
    root->setProperty("puppetFps", std::max(1, view.puppet.fps));
    root->setProperty("puppetMaxPixels", double(view.puppet.maxPixels));
    PuppetItem *item = root->findChild<PuppetItem *>();
    if (item && item != m_puppetItem.data()) { // the Loader made it (the first apply that has puppetMode on, or a restart of the window)
        m_puppetItem = item;
        connect(item, &PuppetItem::hitMask, this, &PetWindow::onPuppetHitMask);
        connect(item, &PuppetItem::engineCrashed, this, [this](bool afterLongRun) {
            if (m_callbacks.puppetCrashed)
                m_callbacks.puppetCrashed(afterLongRun);
        });
        connect(item, &PuppetItem::engineRefused, this, [this](const QString &name) {
            if (m_callbacks.puppetRefused)
                m_callbacks.puppetRefused(name);
        });
    }
    if (item && view.puppet.playSerial != m_puppetSerial) { // (without the item - the Loader has not made it yet - the request waits for the next apply)
        m_puppetSerial = view.puppet.playSerial;
        if (view.puppet.group.isEmpty())
            item->stopMotion(); // the state went back to the base: whatever loops ends
        else
            item->playMotion(view.puppet.group, QStringLiteral("normal"));
    }
}

void PetWindow::onPuppetHitMask(int cols, int rows, int cell, const QByteArray &bits)
{
    m_puppetMasks = maskSetFromHitMask(cols, rows, cell, bits);
    if (m_puppet) {
        m_viewMasks = m_puppetMasks;
        updateInputMasks();
    }
}

void PetWindow::onFrameChanged(int frame)
{
    m_input.setFrame(frame);
}

void PetWindow::onWrapped(qint64 monotonicNs, qint64 loop)
{
    if (m_callbacks.wrapped)
        m_callbacks.wrapped(monotonicNs, loop);
}

void PetWindow::onQmlWrapped(double monotonicNs, double loop)
{
    onWrapped(qint64(monotonicNs), qint64(loop));
}

void PetWindow::onWrapCancelled()
{
    if (m_callbacks.wrapCancelled)
        m_callbacks.wrapCancelled();
}

void PetWindow::onClicked()
{
    if (m_arrange)
        return; // arranging a pet is not clicking it
    if (m_puppetItem && m_puppet)
        clickPuppet();
    if (m_callbacks.clicked)
        m_callbacks.clicked();
}

// A click on a puppet: where it fell on the picture, as fractions of it, turned back through the pet's rotation and flips (pointInPet); the engine decides what it means (the hit area it is on, else a tap group).
void PetWindow::clickPuppet()
{
    const QSize size = petDisplaySize(m_width, m_native);
    // The pet's rectangle in the window: the window is the output when it is expanded (the pet at its place), else the footprint of the pet, which the pet is centred in.
    const PetRect pet = m_expanded ? PetRect{m_x, m_y, double(size.width()), double(size.height())}
                                   : PetRect{(this->width() - size.width()) / 2.0, (this->height() - size.height()) / 2.0, double(size.width()), double(size.height())};
    const auto local = pointInPet({m_lastPress.x(), m_lastPress.y()}, pet, m_rotation, m_flipH, m_flipV);
    if (!local)
        return;
    m_puppetItem->clickAt(local->x / pet.w, local->y / pet.h, m_puppetTapGroups);
}

void PetWindow::applyOpacity()
{
    if (QQuickItem *root = rootObject())
        root->setProperty("petOpacity", m_basePetOpacity * m_input.opacityFactor());
}

void PetWindow::syncInputGeometry()
{
    // The window of the pet: the footprint of its rectangle (the pet's own when it is not turned), as the layout placed it.
    const QSize size = petDisplaySize(m_width, m_native);
    const PetRect footprint = petWindowRect({m_x, m_y, double(size.width()), double(size.height())}, m_rotation);
    const QSize output = screen() ? screen()->size() : size;
    m_input.setGeometry(PetRect{double(std::lround(footprint.x)), double(std::lround(footprint.y)), double(petCeilPx(footprint.w)),
                                double(petCeilPx(footprint.h))},
                        PetSize{double(output.width()), double(output.height())});
}

void PetWindow::applyInputRegion(QPoint offset)
{
    const std::vector<MaskRect> &rects = m_input.regionRects();
    const bool take = inputActive() && m_input.region() != InputRegion::Empty && !rects.empty();
    if (m_appliedKnown && m_appliedHaveRegion == take && m_appliedOffset == offset && (!take || m_appliedRects == rects))
        return; // nothing new: a repeated region would only cost the compositor
    // While a pet is dragged its region only moves, with every motion event: hand it to the compositor at most 30 times a second (the pointer is
    // grabbed, so a region a few frames behind costs nothing; the last position is applied when the timer ends).
    if (m_appliedKnown && take && m_appliedHaveRegion && m_appliedRects == rects && m_input.layout() == PetLayout::Drag && m_regionClock.isValid()
        && m_regionClock.elapsed() < PetInput::kRegionMinIntervalMs) {
        if (!m_regionDeferred.isActive())
            m_regionDeferred.start(int(PetInput::kRegionMinIntervalMs - m_regionClock.elapsed()));
        return;
    }
    m_regionClock.restart();
    if (debugInput())
        qInfo("murasaki-overlay: input region of %s: %s, %d rectangles, offset %d,%d, window %dx%d", qPrintable(m_petId), take ? "takes input" : "click-through",
              int(rects.size()), offset.x(), offset.y(), width(), height());
    m_appliedKnown = true;
    m_appliedHaveRegion = take;
    m_appliedOffset = offset;
    m_appliedRects = take ? rects : std::vector<MaskRect>();
    if (!take) {
        setMask(QRegion());
        setFlag(Qt::WindowTransparentForInput, true);
        return;
    }
    std::vector<QRect> qrects;
    qrects.reserve(rects.size());
    for (const MaskRect &r : rects)
        qrects.emplace_back(r.x + offset.x(), r.y + offset.y(), r.w, r.h);
#ifdef MURASAKI_HAVE_X11
    if (m_x11) {
        // X11: the INPUT shape only; setMask() would clip what is drawn as well and cut the faint edges of the pet.
        setMask(QRegion());
        setFlag(Qt::WindowTransparentForInput, false); // (first: Qt resets the input shape when this changes)
        if (murasaki::x11::setInputShape(this, QList<QRect>(qrects.begin(), qrects.end())))
            return;
    }
#endif
    QRegion region;
    region.setRects(qrects.data(), int(qrects.size()));
    setMask(region);
    setFlag(Qt::WindowTransparentForInput, false);
    requestUpdate(); // the compositor learns the new input region with the next commit
}

void PetWindow::onInputChanged()
{
    applyOpacity();
    applyLayout();
    // A click through the faded pet: unmapped, no compositor keeps the pointer focus on it, so the next clicks reach the window
    // below even with the pointer still (docs/pets/pointer-facts.md, U1). Mapped again when the release ends.
    if (m_input.released() != m_releaseUnmapped && m_shown) {
        m_releaseUnmapped = m_input.released();
        if (!m_viewHidden) // (hidden by the user: stays unmapped)
            setVisible(!m_releaseUnmapped);
    }
    const bool wants = m_input.wantsGlobalPointer();
    if (wants != m_wantedGlobal) {
        m_wantedGlobal = wants;
        if (m_callbacks.pointerWantChanged)
            m_callbacks.pointerWantChanged();
    }
}

// A dodge, or the pet following the pointer in a drag: the pet's place changes and goes to the daemon like an edit of the arrange mode,
// at most once in kCommitDelayMs however often it moves.
void PetWindow::onInputMove(double dx, double dy)
{
    m_x += dx;
    m_y += dy;
    m_editing = true;
    if (m_input.layout() == PetLayout::Drag)
        m_dragMoved = true;
    if (!m_commitTimer.isActive())
        m_commitTimer.start();
    applyLayout();
}

void PetWindow::globalPointer(QPointF position)
{
    if (m_puppetLook && screen() && m_puppetItem) {
        // The gaze: the cursor relative to the pet's picture, in device pixels (the engine scales it by the pet's size).
        const QPointF inOutput = position - QPointF(screen()->geometry().topLeft());
        const double dpr = devicePixelRatio();
        m_puppetItem->pointerAt((inOutput.x() - m_x) * dpr, (inOutput.y() - m_y) * dpr);
    }
    if (!inputActive() || !screen())
        return;
    // The cursor is in the compositor's global logical space; PetInput works in this pet's output.
    m_input.globalPointer(position - QPointF(screen()->geometry().topLeft()));
}

bool PetWindow::event(QEvent *event)
{
    if (debugInput() && (event->type() == QEvent::Enter || event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress
                         || event->type() == QEvent::MouseButtonRelease))
        qInfo("murasaki-overlay: pet %s event %d (input active: %d)", qPrintable(m_petId), int(event->type()), int(inputActive()));
    if (inputActive()) {
        if (event->type() == QEvent::Enter)
            m_input.enter(static_cast<QEnterEvent *>(event)->position(), m_expanded);
        else if (event->type() == QEvent::Leave)
            m_input.leave();
    }
    return QQuickView::event(event);
}

void PetWindow::mousePressEvent(QMouseEvent *event)
{
    // (also with an empty region: sway and Hyprland give a press to the surface the pointer last moved onto, and a faded pet that
    // gets one lets go of the place, PetInteraction F11)
    if (inputActive()) {
        if (event->button() == Qt::LeftButton) {
            m_dragMoved = false;
            m_lastPress = event->position();
#ifdef MURASAKI_HAVE_RELATIVE_POINTER
            if (auto *relative = murasaki::RelativePointer::instance())
                relative->attach();
#endif
            m_input.press(event->position(), m_expanded);
        }
        event->accept();
        return;
    }
    QQuickView::mousePressEvent(event);
}

void PetWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (inputActive() && !(flags() & Qt::WindowTransparentForInput)) {
        if (debugInput() && m_input.layout() == PetLayout::Drag)
            qInfo("murasaki-overlay: pet %s drag motion %.1f,%.1f (window %dx%d)", qPrintable(m_petId), event->position().x(),
                  event->position().y(), width(), height());
        m_input.motion(event->position(), m_expanded);
        event->accept();
        return;
    }
    QQuickView::mouseMoveEvent(event);
}

void PetWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (inputActive() && event->button() == Qt::LeftButton) {
        const bool wasDragging = m_input.layout() == PetLayout::Drag;
        m_input.release();
        if (wasDragging && m_dragMoved) { // the pet stays where it was dropped; the daemon hears it now
            m_commitTimer.stop();
            commit();
        } else if (wasDragging) { // a click that did not move it: nothing to tell the daemon
            m_commitTimer.stop();
            m_editing = false;
            applyLayout();
        }
        m_dragMoved = false;
        event->accept();
        return;
    }
    QQuickView::mouseReleaseEvent(event);
}

} // namespace murasaki
