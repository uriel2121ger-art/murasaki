#include "pointercapability.h"

#include "petbehaviours.h"

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

PointerCapability local(const QString &reason)
{
    PointerCapability c;
    c.backend = PointerBackend::Local;
    c.level = PointerSource::Level::Local;
    c.reason = reason;
    c.degraded.push_back(degrade::kPointerLocal);
    return c;
}

PointerCapability global(PointerBackend backend, const QString &reason)
{
    PointerCapability c;
    c.backend = backend;
    c.level = PointerSource::Level::Global;
    c.reason = reason;
    return c;
}

} // namespace

PointerCapability choosePointerBackend(const CompositorProfile &profile, const PointerEnvironment &environment)
{
    const QString override = environment.override.trimmed().toLower();
    if (override == u"local"_s)
        return local(u"forced by MURASAKI_POINTER=local"_s);
    if (environment.x11Session || profile.family == CompositorFamily::X11)
        return global(PointerBackend::X11Poll, u"X11: the cursor is polled"_s);
    switch (profile.family) {
    case CompositorFamily::Kde:
        if (environment.kwinOnBus)
            return global(PointerBackend::KWinScript, u"KWin: a script reports workspace.cursorPos"_s);
        return local(u"KDE session but org.kde.KWin is not on the bus"_s);
    case CompositorFamily::Hyprland:
        if (!environment.hyprlandSocketDir.isEmpty())
            return global(PointerBackend::HyprlandIpc, u"Hyprland: j/cursorpos over the request socket"_s);
        return local(u"Hyprland without its sockets"_s);
    case CompositorFamily::Gnome:
        return local(u"GNOME: no layer shell and no cursor for clients (the extension may report it later)"_s);
    case CompositorFamily::Sway:
    case CompositorFamily::Wlroots:
    case CompositorFamily::Cosmic:
        return local(u"this compositor gives clients no cursor position"_s);
    case CompositorFamily::X11:
    case CompositorFamily::Unknown:
        break;
    }
    return local(u"unknown compositor"_s);
}

} // namespace murasaki
