#include "x11state.h"

namespace murasaki {
namespace {

QString desktopId(quint32 index)
{
    return QString::number(index);
}

// The output a window overlaps most, or an empty name.
QString outputOf(const X11Window &window, const QList<X11Output> &outputs)
{
    QString best;
    qint64 bestArea = 0;
    for (const X11Output &output : outputs) {
        const QRect common = output.geometry.intersected(window.geometry);
        const qint64 area = qint64(qMax(0, common.width())) * qMax(0, common.height());
        if (area > bestArea) {
            bestArea = area;
            best = output.name;
        }
    }
    return best;
}

} // namespace

SessionState buildX11State(const X11Snapshot &snapshot)
{
    SessionState state;
    const int count = qMax(1, snapshot.desktopCount);
    for (int i = 0; i < count; ++i) {
        const QString name = i < snapshot.desktopNames.size() && !snapshot.desktopNames.at(i).isEmpty()
            ? snapshot.desktopNames.at(i)
            : QStringLiteral("Desktop %1").arg(i + 1);
        state.desktops.append({desktopId(quint32(i)), name, i + 1});
    }
    const QString current = desktopId(snapshot.currentDesktop < quint32(count) ? snapshot.currentDesktop : 0);
    state.defaultDesktop = current;
    for (const X11Output &output : snapshot.outputs)
        state.outputs[output.name] = OutputSession{current, false};

    const auto isOwn = [&](const X11Window &w) { return snapshot.ownPid != 0 && w.pid == snapshot.ownPid; };
    for (const X11Window &window : snapshot.windows) {
        if (isOwn(window) || !window.normal)
            continue;
        addOpenApp(state.openApps, window.resourceClass); // (any desktop, minimized too)
        const bool onCurrentDesktop = window.desktop == kX11AllDesktops || desktopId(window.desktop) == current;
        if (!onCurrentDesktop || window.hidden)
            continue;
        if (window.fullscreen)
            addOpenApp(state.fullscreenApps, window.resourceClass);
        const QString output = outputOf(window, snapshot.outputs);
        if (output.isEmpty())
            continue;
        QRect outputRect;
        for (const X11Output &o : snapshot.outputs) {
            if (o.name == output)
                outputRect = o.geometry;
        }
        if (window.fullscreen || window.maximizedBoth || window.geometry.contains(outputRect))
            state.outputs[output].occluded = true;
        if (window.fullscreen && !state.outputs[output].fullscreen) {
            state.outputs[output].fullscreen = true;
            state.outputs[output].fullscreenApp = window.resourceClass.left(kMaxAppClassLength);
        }
    }
    for (const X11Window &window : snapshot.windows) {
        if (window.id == snapshot.activeWindow && snapshot.activeWindow != 0 && !isOwn(window) && window.normal)
            state.activeApp = window.resourceClass.left(kMaxAppClassLength);
    }
    return state;
}

} // namespace murasaki
