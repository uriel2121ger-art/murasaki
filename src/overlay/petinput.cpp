#include "petinput.h"

#include <algorithm>
#include <cmath>

namespace murasaki {

namespace {

PetInteraction::Level interactionLevel(PointerSource::Level level)
{
    return level == PointerSource::Level::Global ? PetInteraction::Level::Global : PetInteraction::Level::Local;
}

bool turned(double rotation, bool flipH, bool flipV)
{
    return normalizeRotation(rotation) != 0.0 || flipH || flipV;
}

} // namespace

PetInput::PetInput(QObject *parent)
    : QObject(parent)
    , m_interaction(PetInteraction::Level::Global)
{
    m_clock.start();
    m_tick.setInterval(kTickMs);
    m_tick.setTimerType(Qt::PreciseTimer);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        m_interaction.tick(now());
        afterInput();
    });
    m_regionTimer.setSingleShot(true);
    connect(&m_regionTimer, &QTimer::timeout, this, [this] {
        computeRects();
        if (m_rects != m_lastRects) {
            m_lastRects = m_rects;
            m_lastRegionAt = now();
            emit changed();
        }
    });
    m_interaction.setMask(maskTest([this] { return effectiveMask(); }, {1, 1}));
}

// ---- configuration ----------------------------------------------------------------------------------------------------

void PetInput::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    if (!on) { // a pet that is edited or hidden lets go of the pointer: a drag in progress ends, a hover ends
        const bool wasDragging = m_interaction.dragging();
        m_interaction.release(now());
        m_interaction.takeClick(); // letting go because the window is being edited or hidden is not a click
        if (wasDragging)
            emit pointerEvent({PetPointerEvent::Kind::Release});
        m_interaction.leave(now());
    }
    m_enabled = on;
    updateTimer();
    m_rectsDirty = true;
    emitIfChanged(true);
}

void PetInput::setLevel(PointerSource::Level level)
{
    if (level == m_level)
        return;
    m_level = level;
    m_interaction.setLevel(interactionLevel(level), now());
    m_interaction.setConfig(m_config, now());
    updateTimer();
    afterInput();
}

void PetInput::setBehaviour(const PetBehaviourConfig &config)
{
    if (config == m_config)
        return;
    m_config = config;
    m_interaction.setConfig(config, now());
    updateTimer();
    afterInput();
}

void PetInput::setGeometry(PetRect window, PetSize output)
{
    const bool resized = window.w != m_window.w || window.h != m_window.h;
    m_window = window;
    m_output = output;
    m_interaction.setGeometry(window, PetRect{0, 0, output.w, output.h});
    if (resized) {
        m_interaction.setMask(maskTest([this] { return effectiveMask(); }, {window.w, window.h}));
        m_rectsDirty = true;
        emitIfChanged(true);
    }
}

void PetInput::setMasks(std::shared_ptr<const MaskSet> masks, bool frameTracking)
{
    if (masks == m_masks && frameTracking == m_frameTracking)
        return;
    m_masks = std::move(masks);
    m_frameTracking = frameTracking;
    m_turned.clear();
    m_rectsDirty = true;
    emitIfChanged(true);
}

void PetInput::setFrame(int frame)
{
    if (frame == m_frame)
        return;
    m_frame = frame;
    if (m_frameTracking && m_masks && m_masks->frameCount() > 0)
        scheduleRegion();
}

void PetInput::setTransform(double rotation, bool flipH, bool flipV)
{
    if (rotation == m_rotation && flipH == m_flipH && flipV == m_flipV)
        return;
    m_rotation = rotation;
    m_flipH = flipH;
    m_flipV = flipV;
    m_turned.clear();
    m_rectsDirty = true;
    emitIfChanged(true);
}

// ---- masks --------------------------------------------------------------------------------------------------------------

// The mask of what is on screen: the current frame's, or the union of all the frames (video path, or a set of the union alone).
std::shared_ptr<const AlphaMask> PetInput::effectiveMask() const
{
    if (!m_masks || m_masks->isNull())
        return nullptr;
    const bool unionOnly = !m_frameTracking || m_masks->frameCount() == 0;
    const int key = unionOnly ? -1 : m_frame;
    const AlphaMask *base = unionOnly ? &m_masks->silhouette() : &m_masks->maskFor(m_frame);
    const std::shared_ptr<const AlphaMask> alias(m_masks, base);
    if (!turned(m_rotation, m_flipH, m_flipV))
        return alias;
    for (const auto &[k, mask] : m_turned) {
        if (k == key)
            return mask;
    }
    if (m_turned.size() >= 64)
        m_turned.clear();
    auto made = std::make_shared<const AlphaMask>(base->transformed(m_rotation, m_flipH, m_flipV));
    m_turned.emplace_back(key, made);
    return made;
}

void PetInput::computeRects()
{
    m_rectsDirty = false;
    m_rects.clear();
    if (!m_enabled || region() == InputRegion::Empty)
        return;
    const int w = std::max(1, int(std::ceil(m_window.w)));
    const int h = std::max(1, int(std::ceil(m_window.h)));
    const bool unionRegion = region() == InputRegion::Union;
    std::shared_ptr<const AlphaMask> mask;
    if (unionRegion && m_masks && !m_masks->isNull()) {
        // The union of all the frames: a faded pet's region does not change while nobody sees it.
        const std::shared_ptr<const AlphaMask> alias(m_masks, &m_masks->silhouette());
        mask = turned(m_rotation, m_flipH, m_flipV) ? std::make_shared<const AlphaMask>(alias->transformed(m_rotation, m_flipH, m_flipV)) : alias;
    } else {
        mask = effectiveMask();
    }
    if (!mask || mask->isNull()) { // no mask yet: the whole rectangle takes input, as the arrange mode does
        m_rects.push_back({0, 0, w, h});
        return;
    }
    const bool perFrame = !unionRegion && m_frameTracking && m_masks && m_masks->frameCount() > 0;
    const AlphaMask::Rects rects = mask->toRects(perFrame ? kMaxFrameRects : kMaxFixedRects);
    const double sx = double(w) / mask->spaceWidth();
    const double sy = double(h) / mask->spaceHeight();
    m_rects.reserve(rects.rects.size());
    for (const MaskRect &r : rects.rects) {
        const int x0 = std::clamp(int(std::floor(r.x * sx)), 0, w);
        const int y0 = std::clamp(int(std::floor(r.y * sy)), 0, h);
        const int x1 = std::clamp(int(std::ceil((r.x + r.w) * sx)), 0, w);
        const int y1 = std::clamp(int(std::ceil((r.y + r.h) * sy)), 0, h);
        if (x1 > x0 && y1 > y0)
            m_rects.push_back({x0, y0, x1 - x0, y1 - y0});
    }
}

const std::vector<MaskRect> &PetInput::regionRects()
{
    if (m_rectsDirty)
        computeRects();
    return m_rects;
}

void PetInput::scheduleRegion()
{
    if (!m_enabled || region() != InputRegion::Mask || m_regionTimer.isActive())
        return;
    const qint64 wait = std::max<qint64>(0, kRegionMinIntervalMs - (now() - m_lastRegionAt));
    m_regionTimer.start(int(wait));
}

// ---- events ---------------------------------------------------------------------------------------------------------------

QPointF PetInput::local(QPointF pos, bool windowIsOutput) const
{
    if (m_interaction.layout() == PetLayout::Drag)
        return pos; // the grown window's, or still the small one's: the state machine works out which (F12)
    return windowIsOutput ? QPointF(pos.x() - m_window.x, pos.y() - m_window.y) : pos;
}

void PetInput::enter(QPointF pos, bool windowIsOutput)
{
    if (!m_enabled)
        return;
    const QPointF p = local(pos, windowIsOutput);
    m_interaction.enter(now(), {p.x(), p.y()});
    afterInput();
}

void PetInput::leave()
{
    if (!m_enabled)
        return;
    m_interaction.leave(now());
    afterInput();
}

void PetInput::motion(QPointF pos, bool windowIsOutput)
{
    if (!m_enabled)
        return;
    const QPointF p = local(pos, windowIsOutput);
    m_interaction.motionLocal(now(), {p.x(), p.y()});
    afterInput();
}

void PetInput::relativeMotion(double dx, double dy)
{
    if (!m_enabled)
        return;
    m_interaction.motionRelative(now(), dx, dy);
}

void PetInput::press(QPointF pos, bool windowIsOutput)
{
    if (!m_enabled)
        return;
    const QPointF p = local(pos, windowIsOutput);
    const bool wasDragging = m_interaction.dragging();
    m_interaction.press(now(), {p.x(), p.y()});
    if (!wasDragging && m_interaction.dragging())
        emit pointerEvent({PetPointerEvent::Kind::Grab});
    afterInput();
}

void PetInput::release()
{
    if (!m_enabled)
        return;
    const bool wasDragging = m_interaction.dragging();
    m_interaction.release(now());
    const bool click = m_interaction.takeClick();
    afterInput();
    if (wasDragging)
        emit pointerEvent({PetPointerEvent::Kind::Release});
    if (click)
        emit clicked();
}

void PetInput::globalPointer(QPointF position)
{
    if (!m_enabled || m_level != PointerSource::Level::Global)
        return;
    m_interaction.motionGlobal(now(), {position.x(), position.y()});
    m_interaction.tick(now()); // react to the movement now, the tick catches up with the pointer that rests
    afterInput();
}

// ---- outputs ----------------------------------------------------------------------------------------------------------------

bool PetInput::needsPointer() const
{
    return m_enabled && m_interaction.needsPointer();
}

bool PetInput::wantsGlobalPointer() const
{
    return needsPointer() && m_level == PointerSource::Level::Global;
}

double PetInput::opacityFactor() const
{
    return m_enabled ? m_interaction.opacityTarget() : 1.0;
}

InputRegion PetInput::region() const
{
    return m_enabled ? m_interaction.inputRegion() : InputRegion::Empty;
}

PetLayout PetInput::layout() const
{
    return m_enabled ? m_interaction.layout() : PetLayout::Small;
}

bool PetInput::released() const
{
    return m_enabled && m_interaction.released();
}

PetInputStatus PetInput::status() const
{
    PetInputStatus s;
    s.pointer = m_level == PointerSource::Level::Global ? QStringLiteral("global") : QStringLiteral("local");
    s.hidden = m_enabled && m_interaction.hidden();
    if (m_masks)
        s.maskBytes = qint64(m_masks->bytes());
    const bool usesMask = !m_config.clickThrough || m_config.hoverHide || m_config.focusFade || m_config.smartDodge;
    if (!m_masks || m_masks->isNull()) {
        s.hitMask = QStringLiteral("none");
        if (usesMask)
            s.degraded.push_back(degrade::kHitMaskNone);
    } else if (!m_frameTracking || m_masks->quality() == MaskQuality::Silhouette) {
        s.hitMask = QStringLiteral("silhouette");
        if (usesMask)
            s.degraded.push_back(degrade::kHitMaskSilhouette);
    } else {
        s.hitMask = QStringLiteral("frame");
        if (usesMask && m_masks->quality() == MaskQuality::Coarse)
            s.degraded.push_back(degrade::kHitMaskCoarse);
    }
    for (const std::string_view name : m_interaction.degraded())
        s.degraded.push_back(name);
    return s;
}

// ---- after every input -----------------------------------------------------------------------------------------------------

void PetInput::updateTimer()
{
    const bool run = m_enabled && m_interaction.needsPointer();
    if (run && !m_tick.isActive())
        m_tick.start();
    else if (!run && m_tick.isActive())
        m_tick.stop();
}

void PetInput::afterInput()
{
    if (const auto move = m_interaction.takeMove()) {
        // The same step is the drag following the pointer, or a dodge: the states tell them apart. (release() ends the drag before this runs and leaves no step
        // behind; a step that did remain would be told as a Dodge, which the machine ignores while it believes in a drag.)
        const bool drag = m_interaction.dragging();
        emit pointerEvent({drag ? PetPointerEvent::Kind::DragMove : PetPointerEvent::Kind::Dodge, move->dx, move->dy,
                           drag ? 0 : PetBehaviourTuning{}.dodgeCooldownMs});
        emit moveRequested(move->dx, move->dy);
    }
    emitIfChanged(false);
}

void PetInput::emitIfChanged(bool force)
{
    const double opacity = opacityFactor();
    const InputRegion kind = region();
    const PetLayout layout = this->layout();
    const bool released = this->released();
    if (!force && opacity == m_lastOpacity && kind == m_lastRegion && layout == m_lastLayout && released == m_lastReleased)
        return;
    const bool kindChanged = kind != m_lastRegion || layout != m_lastLayout;
    m_lastOpacity = opacity;
    m_lastRegion = kind;
    m_lastLayout = layout;
    m_lastReleased = released;
    if (kindChanged || force || m_rectsDirty)
        computeRects();
    m_lastRects = m_rects;
    m_lastRegionAt = now();
    emit changed();
}

} // namespace murasaki
