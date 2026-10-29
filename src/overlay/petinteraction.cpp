#include "petinteraction.h"

#include <algorithm>
#include <cmath>

namespace murasaki {

namespace {
bool finite(PetPoint p)
{
    return std::isfinite(p.x) && std::isfinite(p.y);
}
} // namespace

PetInteraction::PetInteraction(Level level, PetBehaviourTuning tuning)
    : m_level(level)
    , m_tuning(tuning)
{
}

bool PetInteraction::fadeOn() const
{
    return m_config.hoverHide || m_config.focusFade;
}

bool PetInteraction::needsPointer() const
{
    return fadeOn() || m_config.smartDodge;
}

bool PetInteraction::hit(PetPoint local) const
{
    if (!finite(local))
        return false;
    if (local.x < 0 || local.y < 0 || local.x >= m_window.w || local.y >= m_window.h)
        return false;
    return !m_mask || m_mask(local);
}

void PetInteraction::setLevel(Level level, std::int64_t)
{
    if (level == m_level)
        return;
    m_level = level;
    endDrag();
    m_cursor.reset();
    m_inside = false;
    m_faded = false;
    m_releaseUntil.reset();
    m_releasePressed = false;
    m_move.reset();
    m_dodgePending = false;
}

// A release that has run its time: the region comes back. If the compositor told us in the meantime that the pointer left
// (F11: the next motion after the press does), the pet is not under it any more.
void PetInteraction::expireRelease(std::int64_t nowMs)
{
    if (!m_releaseUntil || nowMs < *m_releaseUntil)
        return;
    m_releaseUntil.reset();
    if (!m_inside)
        m_faded = false;
}

void PetInteraction::setConfig(const PetBehaviourConfig &config, std::int64_t nowMs)
{
    expireRelease(nowMs);
    m_config = config;
    if (m_config.clickThrough)
        m_clickCandidate = false;
    if (!fadeOn()) {
        m_faded = false;
        m_releaseUntil.reset();
        m_releasePressed = false;
    }
    if (!m_config.smartDodge)
        m_dodgePending = false;
    // A pet that can no longer be dragged lets go of the pointer.
    if (m_dragging && (m_config.clickThrough || m_config.locked)) {
        endDrag();
        m_clickCandidate = false;
    }
    if (m_level == Level::Global)
        evaluateGlobal(nowMs);
    else if (fadeOn() && m_inside && !m_dragging)
        m_faded = true; // turned on under the pointer: the surface already holds it
}

void PetInteraction::setGeometry(PetRect window, PetRect output)
{
    m_window = window;
    m_output = output;
    m_dodgePending = false; // the previous move, if any, has been applied
}

void PetInteraction::setMask(MaskTest opaque)
{
    m_mask = std::move(opaque);
}

bool PetInteraction::dodgeReady(std::int64_t nowMs) const
{
    return m_config.smartDodge && !m_config.locked && !m_dragging && !m_dodgePending
        && (!m_lastDodge || nowMs - *m_lastDodge >= m_tuning.dodgeCooldownMs);
}

// One step away from the cursor along the line from it to the window's centre, clamped inside the output.
void PetInteraction::dodgeFrom(std::int64_t nowMs, PetPoint cursor)
{
    const PetPoint c = m_window.center();
    double vx = c.x - cursor.x;
    double vy = c.y - cursor.y;
    double len = std::hypot(vx, vy);
    if (len < 1e-9) {
        vx = 1;
        vy = 0;
        len = 1;
    }
    // max(w, h) // 3 as whole pixels, then clamped to the tuning's range (a range with min > max gives min).
    const double third = std::floor(std::floor(std::max(m_window.w, m_window.h)) / std::max(1, m_tuning.dodgeStepDivisor));
    const double step = clampSafe(third, m_tuning.dodgeStepMin, m_tuning.dodgeStepMax);
    const PetRect target = clampInsideOutput(m_window.translated(vx / len * step, vy / len * step), m_output);
    const double dx = std::round(target.x - m_window.x);
    const double dy = std::round(target.y - m_window.y);
    if (dx == 0 && dy == 0)
        return; // cornered: no step, and no cooldown spent
    m_lastDodge = nowMs;
    m_dodgePending = true;
    PetMove move = m_move.value_or(PetMove{});
    move.dx += dx;
    move.dy += dy;
    m_move = move;
}

void PetInteraction::evaluateGlobal(std::int64_t nowMs)
{
    if (m_level != Level::Global)
        return;
    if (!m_cursor) {
        m_faded = false;
        return;
    }
    const PetPoint local{m_cursor->x - m_window.x, m_cursor->y - m_window.y};
    if (m_dragging) {
        m_faded = false;
        return;
    }
    m_faded = fadeOn() && hit(local);
    if (dodgeReady(nowMs) && m_window.inflated(m_tuning.dodgeMargin).contains(*m_cursor))
        dodgeFrom(nowMs, *m_cursor);
}

void PetInteraction::tick(std::int64_t nowMs)
{
    expireRelease(nowMs); // the region comes back; the next motion brings enter if the pointer is still there
    evaluateGlobal(nowMs);
}

void PetInteraction::motionGlobal(std::int64_t nowMs, PetPoint global)
{
    if (m_level != Level::Global || !finite(global))
        return;
    m_cursor = global;
    evaluateGlobal(nowMs);
}

void PetInteraction::enter(std::int64_t nowMs, PetPoint local)
{
    expireRelease(nowMs);
    m_inside = true;
    if (m_level != Level::Local || m_dragging)
        return;
    if (fadeOn() && hit(local))
        m_faded = true; // a release in progress goes on: the pet stays hidden and the region empty until it ends
    if (dodgeReady(nowMs) && hit(local))
        dodgeFrom(nowMs, {m_window.x + local.x, m_window.y + local.y});
}

void PetInteraction::leave(std::int64_t nowMs)
{
    expireRelease(nowMs);
    m_inside = false;
    if (!m_dragging)
        m_clickCandidate = false; // the pointer left before the button came up
    // During a release the pet stays hidden with its region empty until the release ends (F11: the motion that follows the
    // press makes the compositor send leave while the pointer is still over the pet).
    if (m_level == Level::Local && !m_dragging && !m_releaseUntil)
        m_faded = false;
}

void PetInteraction::motionLocal(std::int64_t nowMs, PetPoint local)
{
    expireRelease(nowMs);
    if (!finite(local))
        return;
    if (m_dragging) {
        const PetMove relative = std::exchange(m_relativePending, PetMove{});
        const PetMove step{local.x - m_dragLastLocal.x, local.y - m_dragLastLocal.y};
        m_dragLastLocal = local;
        // F13: the frame moved between the two events (the local step is not the pointer's): the relative motion says where it is.
        if (m_relativeSeen && std::hypot(step.dx - relative.dx, step.dy - relative.dy) > m_tuning.frameStillPx) {
            moveDragTo({m_dragPointer.x + relative.dx, m_dragPointer.y + relative.dy});
            return;
        }
        if (!m_relativeSeen && nowMs - m_pressAt < m_tuning.dragSettleMs)
            return;
        // F12: the grown window covers the output; the old frame is the small window where the press was.
        const PetPoint grown{m_output.x + local.x, m_output.y + local.y};
        const PetPoint old{m_dragStart.x + local.x, m_dragStart.y + local.y};
        const auto distance = [this](PetPoint p) { return std::hypot(p.x - m_dragPointer.x, p.y - m_dragPointer.y); };
        if (!m_dragGrownFrame && distance(grown) < distance(old))
            m_dragGrownFrame = true;
        moveDragTo(m_dragGrownFrame ? grown : old);
        return;
    }
    if (m_level != Level::Local)
        return;
    m_inside = true;
    // Motion over an opaque pixel of a pet that is not faded yet (the region came back under a still pointer).
    if (fadeOn() && !m_faded && !m_releaseUntil && hit(local))
        m_faded = true;
}

void PetInteraction::motionRelative(std::int64_t nowMs, double dx, double dy)
{
    expireRelease(nowMs);
    if (!m_dragging || !std::isfinite(dx) || !std::isfinite(dy))
        return;
    m_relativeSeen = true;
    m_relativePending.dx += dx;
    m_relativePending.dy += dy;
}

void PetInteraction::moveDragTo(PetPoint pointer)
{
    m_dragPointer = pointer;
    const PetRect wanted = clampToOutput(m_dragStart.translated(pointer.x - m_pressGlobal.x, pointer.y - m_pressGlobal.y), m_output);
    const PetMove total{wanted.x - m_dragStart.x, wanted.y - m_dragStart.y};
    if (total.dx != m_dragReported.dx || total.dy != m_dragReported.dy) {
        PetMove move = m_move.value_or(PetMove{});
        move.dx += total.dx - m_dragReported.dx;
        move.dy += total.dy - m_dragReported.dy;
        m_move = move;
        m_dragReported = total;
    }
}

void PetInteraction::press(std::int64_t nowMs, PetPoint local)
{
    expireRelease(nowMs);
    if (m_dragging || !finite(local))
        return;
    if (m_faded) {
        // The press reached the faded pet (the first click is swallowed): at the local level it is how the pet hears it; at the global
        // level the region is empty already, but sway and Hyprland give a press to the surface the pointer last moved onto (F11).
        // Either way the window lets go of the place so that the next ones pass.
        m_releaseUntil = nowMs + m_tuning.releaseMs;
        m_releasePressed = true;
        return;
    }
    if (m_config.clickThrough || !hit(local))
        return;
    m_clickCandidate = true;
    if (m_config.locked)
        return;
    m_dragging = true;
    m_faded = false;
    m_pressAt = nowMs;
    m_pressGlobal = {m_window.x + local.x, m_window.y + local.y};
    m_dragPointer = m_pressGlobal;
    m_dragGrownFrame = false;
    m_dragLastLocal = local;
    m_relativePending = {};
    m_relativeSeen = false;
    m_dragStart = m_window;
    m_dragReported = {};
}

void PetInteraction::endDrag()
{
    if (m_dragging) {
        // Whether the pointer is on the shrunk window is not announced until it moves: work it out from where the drag left it.
        m_inside = m_dragStart.translated(m_dragReported.dx, m_dragReported.dy).contains(m_dragPointer);
    }
    m_dragging = false;
    m_dragReported = {};
}

void PetInteraction::release(std::int64_t nowMs)
{
    expireRelease(nowMs);
    m_releasePressed = false;
    bool moved = false;
    if (m_dragging) {
        moved = std::hypot(m_dragReported.dx, m_dragReported.dy) > m_tuning.clickSlopPx;
        endDrag();
    }
    if (m_clickCandidate && !moved)
        m_clicked = true;
    m_clickCandidate = false;
}

bool PetInteraction::takeClick()
{
    return std::exchange(m_clicked, false);
}

double PetInteraction::opacityTarget() const
{
    if (!m_faded)
        return 1.0;
    return m_config.hoverHide ? m_tuning.hideOpacity : m_tuning.fadeOpacity;
}

InputRegion PetInteraction::inputRegion() const
{
    if (m_dragging)
        return InputRegion::Mask;
    if (m_faded) {
        if (m_level == Level::Global || m_releaseUntil)
            return InputRegion::Empty;
        return InputRegion::Union;
    }
    // A local pet hears the pointer only through its region: fade and dodge need it, click-through or not.
    const bool needsInput = m_level == Level::Local && needsPointer();
    return (m_config.clickThrough && !needsInput) ? InputRegion::Empty : InputRegion::Mask;
}

PetLayout PetInteraction::layout() const
{
    return m_dragging ? PetLayout::Drag : PetLayout::Small;
}

bool PetInteraction::hidden() const
{
    return m_faded;
}

bool PetInteraction::released() const
{
    return m_releaseUntil && !m_releasePressed;
}

bool PetInteraction::dragging() const
{
    return m_dragging;
}

std::optional<PetMove> PetInteraction::takeMove()
{
    return std::exchange(m_move, std::nullopt);
}

std::vector<std::string_view> PetInteraction::degraded() const
{
    std::vector<std::string_view> names;
    if (m_level != Level::Local)
        return names;
    names.push_back(degrade::kPointerLocal);
    if (fadeOn())
        names.push_back(degrade::kHideClicksSwallowed);
    if (m_config.smartDodge)
        names.push_back(degrade::kDodgeOnlyWhenTouched);
    return names;
}

} // namespace murasaki
