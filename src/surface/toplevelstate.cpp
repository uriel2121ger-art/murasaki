#include "toplevelstate.h"

#include <algorithm>

namespace murasaki {

void ToplevelTracker::added(Id id)
{
    m_windows.insert(id, Window{});
}

void ToplevelTracker::closed(Id id)
{
    m_windows.remove(id);
}

void ToplevelTracker::setAppId(Id id, const QString &appId)
{
    if (auto it = m_windows.find(id); it != m_windows.end())
        it->pendingAppId = appId;
}

void ToplevelTracker::setTitle(Id, const QString &)
{
    // Titles are not used by any rule; the protocol sends them and they are ignored on purpose.
}

void ToplevelTracker::setFlags(Id id, int flags)
{
    if (auto it = m_windows.find(id); it != m_windows.end())
        it->pendingFlags = flags;
}

void ToplevelTracker::outputEntered(Id id, const QString &output)
{
    if (auto it = m_windows.find(id); it != m_windows.end())
        it->pendingOutputs.insert(output);
}

void ToplevelTracker::outputLeft(Id id, const QString &output)
{
    if (auto it = m_windows.find(id); it != m_windows.end())
        it->pendingOutputs.remove(output);
}

void ToplevelTracker::done(Id id)
{
    auto it = m_windows.find(id);
    if (it == m_windows.end())
        return;
    it->appId = it->pendingAppId;
    it->flags = it->pendingFlags;
    it->outputs = it->pendingOutputs;
    it->committed = true;
}

QString ToplevelTracker::appIdOf(Id id) const
{
    const auto it = m_windows.constFind(id);
    return it == m_windows.cend() ? QString() : it->appId;
}

SessionState ToplevelTracker::state(const QStringList &outputs) const
{
    SessionState state;
    for (const QString &output : outputs)
        state.outputs.insert(output, OutputSession{});

    // Deterministic order whatever the hash order: the lowest ids first.
    QList<Id> ids = m_windows.keys();
    std::sort(ids.begin(), ids.end());
    for (const Id id : std::as_const(ids)) {
        const Window &window = m_windows[id];
        if (window.committed)
            addOpenApp(state.openApps, window.appId); // minimized windows are open too
        if (!window.committed || (window.flags & ToplevelMinimized))
            continue;
        if ((window.flags & ToplevelActivated) && state.activeApp.isEmpty())
            state.activeApp = window.appId.left(kMaxAppClassLength);
        const bool fullscreen = window.flags & ToplevelFullscreen;
        if (fullscreen)
            addOpenApp(state.fullscreenApps, window.appId);
        if (fullscreen || (window.flags & ToplevelMaximized)) {
            for (const QString &output : window.outputs) {
                if (auto it = state.outputs.find(output); it != state.outputs.end()) {
                    it->occluded = true;
                    if (fullscreen && !it->fullscreen) {
                        it->fullscreen = true;
                        it->fullscreenApp = window.appId.left(kMaxAppClassLength);
                    }
                }
            }
        }
    }
    return state;
}

} // namespace murasaki
