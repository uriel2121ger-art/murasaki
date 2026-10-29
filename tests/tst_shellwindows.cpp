#include "shellwindows.h"

#include <QFile>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestShellWindows : public QObject
{
    Q_OBJECT

private slots:
    void gnomeSessionsUseShellWindows();
    void otherDesktopsDoNot();
    void theEnvironmentCanForceEitherWay();
    void titlesNameTheOutputAndThePet();
    void theTitleChangesExactlyWhenKeepAboveChanges();
    void theExtensionTrustsTheApplicationIdsTheProcessesSet();
};

static QProcessEnvironment env(const QString &desktop, const QString &force = {})
{
    QProcessEnvironment e;
    if (!desktop.isEmpty())
        e.insert(u"XDG_CURRENT_DESKTOP"_s, desktop);
    if (!force.isEmpty())
        e.insert(u"MURASAKI_SHELL_WINDOWS"_s, force);
    return e;
}

void TestShellWindows::gnomeSessionsUseShellWindows()
{
    QVERIFY(useShellWindows(env(u"GNOME"_s)));
    QVERIFY(useShellWindows(env(u"ubuntu:GNOME"_s)));
    QVERIFY(useShellWindows(env(u"Unity:gnome"_s)));
}

void TestShellWindows::otherDesktopsDoNot()
{
    for (const char *desktop : {"KDE", "sway", "Hyprland", "COSMIC", "X-Cinnamon", "GNOME-Classic-Is-Not-A-Token-Match", ""})
        QVERIFY2(!useShellWindows(env(QString::fromLatin1(desktop))), desktop);
}

void TestShellWindows::theEnvironmentCanForceEitherWay()
{
    QVERIFY(useShellWindows(env(u"KDE"_s, u"1"_s)));
    QVERIFY(!useShellWindows(env(u"GNOME"_s, u"0"_s)));
    QVERIFY(useShellWindows(env(u"GNOME"_s, u"nonsense"_s))); // anything but 0 and 1 is ignored
}

void TestShellWindows::titlesNameTheOutputAndThePet()
{
    QCOMPARE(shellWallpaperTitle(u"DP-1"_s), u"Murasaki Wallpaper | DP-1"_s);
    QCOMPARE(shellPetTitle(u"HDMI-A-1"_s, u"abc"_s), u"Murasaki Pet | HDMI-A-1 | abc"_s);
    QCOMPARE(shellPetTitle(u"HDMI-A-1"_s, u"abc"_s, false), u"Murasaki Pet | HDMI-A-1 | abc"_s);
    QCOMPARE(shellPetTitle(u"HDMI-A-1"_s, u"abc"_s, true), u"Murasaki Pet | HDMI-A-1 | abc | below"_s); // keepAbove off: the extension reads the marker
    // a stage adds its word after the pet id (and after "below"); anything that is not a mode of ours adds nothing
    QCOMPARE(shellPetTitle(u"DP-1"_s, u"abc"_s, false, u"output"_s), u"Murasaki Pet | DP-1 | abc | stage output"_s);
    QCOMPARE(shellPetTitle(u"DP-1"_s, u"abc"_s, false, u"desktop"_s), u"Murasaki Pet | DP-1 | abc | stage desktop"_s);
    QCOMPARE(shellPetTitle(u"DP-1"_s, u"abc"_s, true, u"output"_s), u"Murasaki Pet | DP-1 | abc | below | stage output"_s);
    QCOMPARE(shellPetTitle(u"DP-1"_s, u"abc"_s, false, QString()), u"Murasaki Pet | DP-1 | abc"_s);
    QCOMPARE(shellPetTitle(u"DP-1"_s, u"abc"_s, false, u"off"_s), u"Murasaki Pet | DP-1 | abc"_s);
    QCOMPARE(shellPetTitle(u"DP-1"_s, u"abc"_s, false, u"output | below"_s), u"Murasaki Pet | DP-1 | abc"_s);
}

void TestShellWindows::theTitleChangesExactlyWhenKeepAboveChanges()
{
    // the title starts as "above" (titleBelow false) and follows keepAbove: a change only when they disagree with what it says
    bool titleBelow = false;
    QVERIFY(!shellPetTitleChanges(titleBelow, true));  // keepAbove on, title says above: nothing to do
    QVERIFY(shellPetTitleChanges(titleBelow, false));  // keepAbove off: the title must say below
    titleBelow = true;
    QVERIFY(!shellPetTitleChanges(titleBelow, false)); // already says below
    QVERIFY(shellPetTitleChanges(titleBelow, true));   // keepAbove back on: the marker goes
    // and the titles it produces round trip with the extension's reading (lib/classify.js: the third field, exactly "below")
    QVERIFY(!shellPetTitle(u"DP-1"_s, u"p"_s, false).endsWith(u" | below"_s));
    QVERIFY(shellPetTitle(u"DP-1"_s, u"p"_s, true).endsWith(u" | below"_s));
}

void TestShellWindows::theExtensionTrustsTheApplicationIdsTheProcessesSet()
{
    // the ids are written in two places, lib/classify.js and shellwindows.h (which the two processes use for setDesktopFileName)
    QFile file(QStringLiteral(QT_TESTCASE_SOURCEDIR "/../data/gnome-shell/murasaki@uriel2121ger-art.github.io/lib/classify.js"));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
    const QString js = QString::fromUtf8(file.readAll());
    QVERIFY(js.contains(u"SURFACE_APP_ID = '%1'"_s.arg(QString::fromLatin1(kShellSurfaceAppId))));
    QVERIFY(js.contains(u"OVERLAY_APP_ID = '%1'"_s.arg(QString::fromLatin1(kShellOverlayAppId))));
    QVERIFY(js.contains(u"PET_BELOW_MARKER = '%1'"_s.arg(QString::fromLatin1(kShellPetBelowMarker))));
}

QTEST_GUILESS_MAIN(TestShellWindows)
#include "tst_shellwindows.moc"
