#include "compositorprofile.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestCompositorProfile : public QObject
{
    Q_OBJECT

private slots:
    void plasmaKeepsTheBottomLayerAndKWin();
    void plasmaWithoutKWinOnTheBusIsNotPlasma();
    void swayUsesItsIpcAndGetsAppIdsFromToplevels();
    void swayWithoutToplevelProtocolHasNoAppIds();
    void hyprlandUsesItsIpcAndGetsAppIdsFromToplevels();
    void labwcAndOtherStackingCompositorsUseTheProtocol();
    void aCompositorWithLayerShellButNoOtherHintIsWlrootsStyle();
    void cosmicHasLayerShellButNoStateProtocolYet();
    void gnomeHasNoLayerShell();
    void theDesktopNameDecidesWithoutTheSocketsToo();
    void anX11SessionUsesEwmhWhateverTheDesktop();
    void everyProfileSaysWhatItDid();
};

static const QSet<QString> kWlr{u"zwlr_layer_shell_v1"_s, u"zwlr_foreign_toplevel_manager_v1"_s, u"ext_foreign_toplevel_list_v1"_s};

static QProcessEnvironment env(std::initializer_list<std::pair<QString, QString>> values)
{
    QProcessEnvironment e;
    for (const auto &[key, value] : values)
        e.insert(key, value);
    return e;
}

void TestCompositorProfile::plasmaKeepsTheBottomLayerAndKWin()
{
    const auto p = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"KDE"_s}}), true, {u"zwlr_layer_shell_v1"_s});
    QCOMPARE(p.family, CompositorFamily::Kde);
    QCOMPARE(p.monitor, MonitorKind::KWin);
    QCOMPARE(p.layer, WallpaperLayer::Bottom);
    QVERIFY(!p.appsFromToplevels);
}

void TestCompositorProfile::plasmaWithoutKWinOnTheBusIsNotPlasma()
{
    const auto p = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"KDE"_s}}), false, kWlr);
    QVERIFY(p.family != CompositorFamily::Kde);
}

void TestCompositorProfile::swayUsesItsIpcAndGetsAppIdsFromToplevels()
{
    const auto p = selectCompositorProfile(env({{u"SWAYSOCK"_s, u"/run/sway.sock"_s}, {u"XDG_CURRENT_DESKTOP"_s, u"sway"_s}}), false, kWlr);
    QCOMPARE(p.family, CompositorFamily::Sway);
    QCOMPARE(p.monitor, MonitorKind::Sway);
    QCOMPARE(p.layer, WallpaperLayer::Background);
    QVERIFY(p.appsFromToplevels);
}

void TestCompositorProfile::swayWithoutToplevelProtocolHasNoAppIds()
{
    const auto p = selectCompositorProfile(env({{u"SWAYSOCK"_s, u"/run/sway.sock"_s}}), false, {u"zwlr_layer_shell_v1"_s});
    QCOMPARE(p.monitor, MonitorKind::Sway);
    QVERIFY(!p.appsFromToplevels);
}

void TestCompositorProfile::hyprlandUsesItsIpcAndGetsAppIdsFromToplevels()
{
    const auto p = selectCompositorProfile(env({{u"HYPRLAND_INSTANCE_SIGNATURE"_s, u"abc"_s}}), false, kWlr);
    QCOMPARE(p.family, CompositorFamily::Hyprland);
    QCOMPARE(p.monitor, MonitorKind::Hyprland);
    QVERIFY(p.appsFromToplevels);
}

void TestCompositorProfile::labwcAndOtherStackingCompositorsUseTheProtocol()
{
    const auto p = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"labwc:wlroots"_s}}), false, kWlr);
    QCOMPARE(p.family, CompositorFamily::Wlroots);
    QCOMPARE(p.monitor, MonitorKind::ForeignToplevel);
    QCOMPARE(p.layer, WallpaperLayer::Background);
    QVERIFY(!p.appsFromToplevels); // the monitor already brings them
}

void TestCompositorProfile::aCompositorWithLayerShellButNoOtherHintIsWlrootsStyle()
{
    const auto p = selectCompositorProfile(env({}), false, {u"zwlr_layer_shell_v1"_s});
    QCOMPARE(p.family, CompositorFamily::Wlroots);
    QVERIFY(p.layerShell);
    QCOMPARE(p.monitor, MonitorKind::None); // nothing to learn windows from: wallpapers never pause
}

void TestCompositorProfile::cosmicHasLayerShellButNoStateProtocolYet()
{
    // COSMIC offers ext-foreign-toplevel-list (identity only) and its own state protocol, which is not used yet.
    const auto p = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"COSMIC"_s}}), false,
                                           {u"zwlr_layer_shell_v1"_s, u"ext_foreign_toplevel_list_v1"_s});
    QCOMPARE(p.family, CompositorFamily::Cosmic);
    QVERIFY(p.layerShell);
    QCOMPARE(p.layer, WallpaperLayer::Background);
    QCOMPARE(p.monitor, MonitorKind::None);
}

void TestCompositorProfile::gnomeHasNoLayerShell()
{
    const auto p = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"ubuntu:GNOME"_s}}), false, {});
    QCOMPARE(p.family, CompositorFamily::Gnome);
    QVERIFY(!p.layerShell);
    QVERIFY(p.summary.contains(u"no layer-shell"_s));
    QCOMPARE(p.monitor, MonitorKind::None); // the extension is not on the bus
    // With the Murasaki extension running in the shell, it is the window source.
    const auto withShell = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"ubuntu:GNOME"_s}}), false, {}, true);
    QCOMPARE(withShell.monitor, MonitorKind::GnomeShell);
    QVERIFY(withShell.summary.contains(u"GNOME Shell extension"_s));
}

void TestCompositorProfile::theDesktopNameDecidesWithoutTheSocketsToo()
{
    QCOMPARE(selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"sway"_s}}), false, kWlr).family, CompositorFamily::Sway);
    // No socket: the IPC monitor is not possible, the protocol is.
    QCOMPARE(selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"sway"_s}}), false, kWlr).monitor, MonitorKind::ForeignToplevel);
    QCOMPARE(selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"Hyprland"_s}}), false, kWlr).family, CompositorFamily::Hyprland);
}

void TestCompositorProfile::anX11SessionUsesEwmhWhateverTheDesktop()
{
    for (const char *desktop : {"KDE", "ubuntu:GNOME", "X-Cinnamon", "sway", ""}) {
        const auto p = selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, QString::fromLatin1(desktop)}}), true, kWlr, true, true);
        QCOMPARE(p.family, CompositorFamily::X11);
        QCOMPARE(p.monitor, MonitorKind::X11Ewmh);
        QVERIFY(!p.layerShell);
    }
    // A Wayland session of the same desktop is not X11.
    QCOMPARE(selectCompositorProfile(env({{u"XDG_CURRENT_DESKTOP"_s, u"KDE"_s}}), true, kWlr, false, false).monitor, MonitorKind::KWin);
}

void TestCompositorProfile::everyProfileSaysWhatItDid()
{
    for (const auto &e : {env({{u"XDG_CURRENT_DESKTOP"_s, u"KDE"_s}}), env({{u"SWAYSOCK"_s, u"/x"_s}}), env({})}) {
        const auto p = selectCompositorProfile(e, true, kWlr);
        QVERIFY(!p.summary.isEmpty());
    }
}

QTEST_GUILESS_MAIN(TestCompositorProfile)
#include "tst_compositorprofile.moc"
