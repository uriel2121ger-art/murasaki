// User shortcuts, host side: the key and name rules, the launch gate, the .desktop reader and scanner, the launcher (planning and
// the Exec reader, with hostile lines) and the catalogue model. Nothing starts a process: the launcher is driven with a recording
// function. docs/design/specs/2026-10-21-user-shortcuts-design.md

#include "shortcut/desktopentry.h"
#include "shortcut/launcher.h"
#include "shortcut/launchassigned.h"
#include "shortcut/projectkeys.h"
#include "shortcut/shortcutcatalog.h"
#include "shortcut/shortcutpolicy.h"
#include "testutil.h"
#include "weproperty.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki::shortcut;

namespace {

DesktopEnvironment testEnv(const QStringList &desktops = {QStringLiteral("KDE")})
{
    DesktopEnvironment env;
    env.currentDesktops = desktops;
    env.locales = {QStringLiteral("es_MX"), QStringLiteral("es")};
    env.hasExecutable = [](const QString &p) { return p == QLatin1String("/usr/bin/present") || p == QLatin1String("present"); };
    return env;
}

QByteArray entryText(const QString &extra = {}, const QString &name = QStringLiteral("Firefox"), const QString &exec = QStringLiteral("firefox %u"))
{
    return (QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nExec=%2\nIcon=firefox\n%3").arg(name, exec, extra)).toUtf8();
}

FindProgram finder(const QStringList &present)
{
    return [present](const QString &name) { return present.contains(name) ? QStringLiteral("/usr/bin/") + name : QString(); };
}

} // namespace

class TstShortcut : public QObject
{
    Q_OBJECT

private slots:
    // ---- keys and names ----
    void keysAreBoundedUtf8WithoutControls()
    {
        QVERIFY(validKey("launcher1"));
        QVERIFY(validKey(QStringLiteral("身体：头").toUtf8()));
        QVERIFY(validKey(QByteArray(63, 'a')));
        QVERIFY(!validKey(QByteArray(64, 'a')));
        QVERIFY(!validKey(""));
        QVERIFY(!validKey(QByteArray("a\0b", 3)));
        QVERIFY(!validKey("a\nb"));
        QVERIFY(!validKey("a\x7f"));
        QVERIFY(!validKey("\xff\xfe"));       // not UTF-8
        QVERIFY(!validKey("\xed\xa0\x80"));   // a surrogate
    }

    void displayNamesLoseControlsBidiAndLength()
    {
        QCOMPARE(cleanDisplayName((QStringLiteral("Fire") + QChar(0x202E) + QStringLiteral("fox") + QChar(0x200B) + QChar(7))), QStringLiteral("Firefox"));
        QCOMPARE(cleanDisplayName(QStringLiteral("  a \t\n b  c  ")), QStringLiteral("a b c"));
        QCOMPARE(cleanDisplayName((QString(QChar(0x2066)) + QStringLiteral("x") + QChar(0x2069) + QChar(0xFEFF))), QStringLiteral("x"));
        QCOMPARE(cleanDisplayName(QString()), QString());
        const QString longName = cleanDisplayName(QString(500, QLatin1Char('a')));
        QCOMPARE(longName.toUtf8().size(), 64);
        // a cut never splits a character: 3-byte characters, 64 / 3 = 21 of them
        const QString wide = cleanDisplayName(QString(100, QChar(0x3042)));
        QCOMPARE(wide.size(), 21);
        QVERIFY(!wide.contains(QChar::ReplacementCharacter));
        // emoji (4 bytes, a surrogate pair) at the cut
        const QString emoji = cleanDisplayName(QStringLiteral("a").repeated(62) + QString::fromUcs4(U"\U0001F600"));
        QCOMPARE(emoji.toUtf8().size(), 62);
        // tag characters (they spell hidden text) and variation selectors go, and a stack of combining marks is cut to two
        const QString tags = QString::fromUcs4(U"a\U000E0041\U000E0042b\uFE0F\U000E0100c");
        QCOMPARE(cleanDisplayName(tags), QStringLiteral("abc"));
        QCOMPARE(cleanDisplayName(QStringLiteral("e") + QString(50, QChar(0x0301)) + QStringLiteral("x")), QStringLiteral("e") + QString(2, QChar(0x0301)) + QStringLiteral("x"));
    }

    // ---- the launch gate ----
    void aLaunchNeedsEverythingToHold()
    {
        LaunchLimiter limiter;
        LaunchGate gate(&limiter);
        LaunchRequest r;
        r.nowMs = 10000;
        r.knownKey = true;
        r.assignedId = QStringLiteral("a.desktop");
        QCOMPARE(gate.decide(r), Verdict::NoRecentClick); // no click yet
        gate.noteClick(9500);
        LaunchRequest off = r;
        off.allowed = false;
        QCOMPARE(gate.decide(off), Verdict::SwitchOff);
        LaunchRequest unknown = r;
        unknown.knownKey = false;
        QCOMPARE(gate.decide(unknown), Verdict::UnknownKey);
        LaunchRequest unassigned = r;
        unassigned.assignedId.clear();
        QCOMPARE(gate.decide(unassigned), Verdict::Unassigned);
        LaunchRequest hidden = r;
        hidden.visible = false;
        QCOMPARE(gate.decide(hidden), Verdict::NotVisible);
        QCOMPARE(gate.decide(r), Verdict::Allow);
    }

    void aClickIsUsedOnceAndExpires()
    {
        LaunchLimiter limiter;
        LaunchGate gate(&limiter);
        LaunchRequest r;
        r.knownKey = true;
        r.assignedId = QStringLiteral("a.desktop");
        gate.noteClick(1000);
        r.nowMs = 1800; // 0.8 s later: too old
        QCOMPARE(gate.decide(r), Verdict::NoRecentClick);
        r.nowMs = 1700;
        QCOMPARE(gate.decide(r), Verdict::Allow);
        r.nowMs = 1750; // the same click again: used up
        QCOMPARE(gate.decide(r), Verdict::NoRecentClick);
        // a clock that goes back is no click
        gate.noteClick(50000);
        r.nowMs = 40000;
        QCOMPARE(gate.decide(r), Verdict::NoRecentClick);
    }

    void launchesAreSpacedAndLimited()
    {
        LaunchLimiter limiter; // 6 a minute
        LaunchGate a(&limiter), b(&limiter);
        LaunchRequest r;
        r.knownKey = true;
        r.assignedId = QStringLiteral("a.desktop");
        const auto press = [&](LaunchGate &g, qint64 t) { g.noteClick(t); r.nowMs = t + 10; return g.decide(r); };
        QCOMPARE(press(a, 1000), Verdict::Allow);
        QCOMPARE(press(a, 2000), Verdict::TooSoon);        // within 2 s
        QCOMPARE(press(a, 3500), Verdict::SameAppTooSoon); // 2 s later but the same application within 5 s
        r.assignedId = QStringLiteral("b.desktop");
        QCOMPARE(press(a, 3600), Verdict::Allow);
        // the minute's total, over wallpapers: 6 in all
        int allowed = 2;
        qint64 t = 10000;
        for (int i = 0; i < 20; ++i) {
            r.assignedId = QStringLiteral("app%1.desktop").arg(i);
            LaunchGate &g = i % 2 ? a : b;
            const Verdict v = press(g, t);
            allowed += v == Verdict::Allow ? 1 : 0;
            t += 2500;
            if (t > 50000) {
                break;
            }
        }
        QCOMPARE(allowed, 6);
        // a minute later it is open again
        r.assignedId = QStringLiteral("late.desktop");
        QCOMPARE(press(b, 200000), Verdict::Allow);
    }

    void theGlobalPolicyJudgesWhatTheDaemonKnows()
    {
        GlobalLaunchPolicy policy;
        QCOMPARE(policy.decide(1000, false, QStringLiteral("a.desktop")), Verdict::SwitchOff);
        QCOMPARE(policy.decide(1000, true, QString()), Verdict::Unassigned);
        QCOMPARE(policy.decide(1000, true, QStringLiteral("a.desktop")), Verdict::Allow);
        QCOMPARE(policy.decide(2000, true, QStringLiteral("a.desktop")), Verdict::SameAppTooSoon);
        QCOMPARE(policy.decide(2000, true, QStringLiteral("b.desktop")), Verdict::Allow);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(policy.decide(3000 + i, true, QStringLiteral("app%1.desktop").arg(i)), Verdict::Allow);
        }
        QCOMPARE(policy.decide(4000, true, QStringLiteral("late.desktop")), Verdict::RateLimit); // 6 in a minute
        QCOMPARE(policy.decide(70000, true, QStringLiteral("late.desktop")), Verdict::Allow);
    }

    void theDaemonStartsOnlyWhatIsInstalledAllowedAndNotTooOften()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("applications"));
        testutil::writeFile(root + QStringLiteral("/a.desktop"), entryText({}, QStringLiteral("Alpha")));
        testutil::writeFile(root + QStringLiteral("/b.desktop"), entryText({}, QStringLiteral("Bravo")));
        GlobalLaunchPolicy policy;
        QList<LaunchCommand> started;
        const auto start = [&](const LaunchCommand &c) { started << c; return true; };
        const auto go = [&](const QString &id, qint64 t, bool on = true) {
            return launchAssigned(id, on, policy, t, {root}, testEnv(), finder({QStringLiteral("gio")}), start);
        };
        auto outcome = go(QStringLiteral("a.desktop"), 1000);
        QVERIFY(outcome.started);
        QCOMPARE(started.size(), qsizetype{1});
        QCOMPARE(started[0].program, QStringLiteral("/usr/bin/gio"));
        QVERIFY(started[0].arguments.last().endsWith(QStringLiteral("/applications/a.desktop")));
        // the same one again, too soon; another is fine
        QVERIFY(!go(QStringLiteral("a.desktop"), 2000).started);
        QVERIFY(go(QStringLiteral("b.desktop"), 2000).started);
        // not installed: refused and no count used
        outcome = go(QStringLiteral("gone.desktop"), 3000);
        QVERIFY(!outcome.started);
        QVERIFY(outcome.reason.contains(QStringLiteral("not installed")));
        // the switch off, and an id that is not a file name
        QVERIFY(!go(QStringLiteral("a.desktop"), 100000, false).started);
        QVERIFY(!go(QStringLiteral("../a.desktop"), 100000).started);
        QVERIFY(!go(QString(), 100000).started);
        QCOMPARE(started.size(), qsizetype{2});
        // a launcher that fails is a failure, not a success
        const auto failing = launchAssigned(QStringLiteral("a.desktop"), true, policy, 200000, {root}, testEnv(), finder({QStringLiteral("gio")}),
                                            [](const LaunchCommand &) { return false; });
        QVERIFY(!failing.started);
    }

    void theShortcutKeysOfAProjectAreReadByTheHost()
    {
        QTemporaryDir dir;
        testutil::writeFile(dir.filePath(QStringLiteral("project.json")),
                            "\xEF\xBB\xBF{\"general\":{\"properties\":{\"launcher2\":{\"type\":\"usershortcut\",\"value\":\"\"},"
                            "\"launcher1\":{\"type\":\"UserShortcut\"},\"color\":{\"type\":\"color\"},\"bad\\nkey\":{\"type\":\"usershortcut\"},"
                            "\"plain\":5}}}");
        QCOMPARE(userShortcutKeys(dir.path()), (QStringList{QStringLiteral("launcher1"), QStringLiteral("launcher2")}));
        QCOMPARE(userShortcutKeys(dir.filePath(QStringLiteral("missing"))), QStringList());
        testutil::writeFile(dir.filePath(QStringLiteral("project.json")), "not json");
        QCOMPARE(userShortcutKeys(dir.path()), QStringList());
        // at most kMaxSlots, and a huge file is not read
        QByteArray many = "{\"general\":{\"properties\":{";
        for (int i = 0; i < 100; ++i) {
            many += QByteArray(i ? "," : "") + "\"k" + QByteArray::number(i) + "\":{\"type\":\"usershortcut\"}";
        }
        many += "}}}";
        testutil::writeFile(dir.filePath(QStringLiteral("project.json")), many);
        QCOMPARE(userShortcutKeys(dir.path()).size(), kMaxSlots);
        testutil::writeFile(dir.filePath(QStringLiteral("project.json")), QByteArray(kMaxProjectBytes + 10, ' '));
        QCOMPARE(userShortcutKeys(dir.path()), QStringList());
    }

    // ---- .desktop files ----
    void anApplicationEntryIsRead()
    {
        const auto e = parseDesktopEntry(entryText(QStringLiteral("Name[es]=Navegador\nName[fr]=Fureteur\nTerminal=false\n")), QStringLiteral("firefox.desktop"), testEnv());
        QVERIFY(e);
        QCOMPARE(e->id, QStringLiteral("firefox.desktop"));
        QCOMPARE(e->name, QStringLiteral("Navegador")); // the locale's
        QCOMPARE(e->icon, QStringLiteral("firefox"));
        QCOMPARE(e->exec, QStringLiteral("firefox %u"));
        QVERIFY(!e->terminal);
    }

    void entriesThatAreNotForThePickerAreLeftOut()
    {
        const auto env = testEnv();
        const auto id = QStringLiteral("x.desktop");
        QVERIFY(!parseDesktopEntry("[Desktop Entry]\nType=Link\nName=a\nExec=b\n", id, env));
        QVERIFY(!parseDesktopEntry("[Desktop Entry]\nName=a\nExec=b\n", id, env)); // no Type
        QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("Hidden=true\n")), id, env));
        QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("NoDisplay=true\n")), id, env));
        QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("OnlyShowIn=GNOME;XFCE;\n")), id, env));
        QVERIFY(parseDesktopEntry(entryText(QStringLiteral("OnlyShowIn=GNOME;KDE;\n")), id, env));
        QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("NotShowIn=KDE;\n")), id, env));
        QVERIFY(!parseDesktopEntry(entryText(QStringLiteral("TryExec=/nonexistent/program\n")), id, env));
        QVERIFY(parseDesktopEntry(entryText(QStringLiteral("TryExec=present\n")), id, env));
        QVERIFY(!parseDesktopEntry("[Desktop Entry]\nType=Application\nName=a\n", id, env)); // no Exec
        QVERIFY(!parseDesktopEntry("[Desktop Entry]\nType=Application\nExec=a\n", id, env)); // no Name
        QVERIFY(!parseDesktopEntry("", id, env));
        QVERIFY(!parseDesktopEntry("[Desktop Entry]\nType=Application\nName=a\nExec=b\n\xff\xfe", id, env)); // not UTF-8
        QVERIFY(!parseDesktopEntry(QByteArray("[Desktop Entry]\nType=Application\nName=a\nExec=b\0c\n", 52), id, env)); // a NUL
    }

    void hostileEntriesAreBounded()
    {
        const auto env = testEnv();
        const auto id = QStringLiteral("x.desktop");
        // too big
        QVERIFY(!parseDesktopEntry(entryText() + QByteArray(70 * 1024, '#'), id, env));
        // the first key wins, and a later group (an action) is not read
        const auto e = parseDesktopEntry("[Desktop Entry]\nType=Application\nName=First\nName=Second\nExec=a\n[Desktop Action x]\nName=Third\nExec=evil\n", id, env);
        QVERIFY(e);
        QCOMPARE(e->name, QStringLiteral("First"));
        QCOMPARE(e->exec, QStringLiteral("a"));
        // a name with controls and a bidi override is cleaned, and a long one cut
        const auto cleaned = parseDesktopEntry(entryText({}, (QStringLiteral("A") + QChar(0x202E) + QStringLiteral("b") + QChar(1)) + QString(400, QLatin1Char('x'))), id, env);
        QVERIFY(cleaned);
        QVERIFY(cleaned->name.toUtf8().size() <= 64);
        QVERIFY(!cleaned->name.contains(QChar(0x202E)));
        // an icon with a control character is no icon; a very long one is cut
        const auto icon = parseDesktopEntry("[Desktop Entry]\nType=Application\nName=a\nExec=b\nIcon=ab\x01" "c\n", id, env);
        QVERIFY(icon);
        QVERIFY(icon->icon.isEmpty());
        const auto longIcon = parseDesktopEntry(("[Desktop Entry]\nType=Application\nName=a\nExec=b\nIcon=" + QByteArray(1000, 'i') + "\n"), id, env);
        QVERIFY(longIcon);
        QVERIFY(longIcon->icon.toUtf8().size() <= 256);
        // an Exec line over the cap
        QVERIFY(!parseDesktopEntry("[Desktop Entry]\nType=Application\nName=a\nExec=" + QByteArray(5000, 'e') + "\n", id, env));
        // the string escapes
        const auto esc = parseDesktopEntry("[Desktop Entry]\nType=Application\nName=a\\sb\nExec=c\n", id, env);
        QVERIFY(esc);
        QCOMPARE(esc->name, QStringLiteral("a b"));
    }

    void applicationsAreScannedFromRootsInOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a/applications")), b = dir.filePath(QStringLiteral("b/applications"));
        testutil::writeFile(a + QStringLiteral("/one.desktop"), entryText({}, QStringLiteral("One A")));
        testutil::writeFile(b + QStringLiteral("/one.desktop"), entryText({}, QStringLiteral("One B"))); // shadowed by the first root
        testutil::writeFile(b + QStringLiteral("/two.desktop"), entryText({}, QStringLiteral("Two")));
        testutil::writeFile(b + QStringLiteral("/sub/three.desktop"), entryText({}, QStringLiteral("Three"))); // id sub-three.desktop
        testutil::writeFile(b + QStringLiteral("/hidden.desktop"), entryText(QStringLiteral("Hidden=true\n"), QStringLiteral("Hidden")));
        testutil::writeFile(b + QStringLiteral("/readme.txt"), "not an entry");
        QFile::link(b + QStringLiteral("/two.desktop"), a + QStringLiteral("/link.desktop")); // a link to a regular file counts
        QFile::link(QStringLiteral("/etc/hostname"), a + QStringLiteral("/evil.desktop"));    // a regular file that is no entry: parsed and dropped
        const auto entries = scanApplications({a, b}, testEnv());
        QStringList ids;
        for (const auto &e : entries) {
            ids << e.id;
        }
        ids.sort();
        QCOMPARE(ids, (QStringList{QStringLiteral("link.desktop"), QStringLiteral("one.desktop"), QStringLiteral("sub-three.desktop"), QStringLiteral("two.desktop")}));
        const auto one = findApplication({a, b}, QStringLiteral("one.desktop"), testEnv());
        QVERIFY(one);
        QCOMPARE(one->name, QStringLiteral("One A"));
        QVERIFY(findApplication({a, b}, QStringLiteral("sub-three.desktop"), testEnv()));
        QVERIFY(!findApplication({a, b}, QStringLiteral("hidden.desktop"), testEnv()));
        QVERIFY(!findApplication({a, b}, QStringLiteral("nothing.desktop"), testEnv()));
        // an id is a file name
        for (const char *bad : {"../one.desktop", "a/b.desktop", ".hidden.desktop", "-one.desktop", "--version.desktop", "one", "", "one.desktop/../../x.desktop"}) {
            QVERIFY2(!findApplication({a, b}, QString::fromLatin1(bad), testEnv()), bad);
        }
        // sorted by name
        QCOMPARE(entries.front().name, QStringLiteral("One A"));
    }

    void aHugeDirectoryIsCapped()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("applications"));
        for (int i = 0; i < 1100; ++i) {
            testutil::writeFile(root + QStringLiteral("/app%1.desktop").arg(i, 4, 10, QLatin1Char('0')), entryText({}, QStringLiteral("App %1").arg(i)));
        }
        QCOMPARE(static_cast<int>(scanApplications({root}, testEnv()).size()), kMaxDesktopEntries);
    }

    // ---- Exec lines ----
    void execLinesAreReadBySpecAndFieldCodesDropped()
    {
        QCOMPARE(parseExec(QStringLiteral("firefox %u")).value(), (QStringList{QStringLiteral("firefox")}));
        QCOMPARE(parseExec(QStringLiteral("/usr/bin/app --flag %F %i %c %k")).value(), (QStringList{QStringLiteral("/usr/bin/app"), QStringLiteral("--flag")}));
        QCOMPARE(parseExec(QStringLiteral("app \"two words\" 100%% x")).value(), (QStringList{QStringLiteral("app"), QStringLiteral("two words"), QStringLiteral("100%"), QStringLiteral("x")}));
        QCOMPARE(parseExec(QStringLiteral("app \"a \\\"quoted\\\" \\$HOME \\\\\"")).value(), (QStringList{QStringLiteral("app"), QStringLiteral("a \"quoted\" $HOME \\")}));
        QCOMPARE(parseExec(QStringLiteral("env FOO=1 app")).value(), (QStringList{QStringLiteral("env"), QStringLiteral("FOO=1"), QStringLiteral("app")}));
        // a field code inside quotes goes too, and an argument that was only a code disappears
        QCOMPARE(parseExec(QStringLiteral("app \"x%uy\" %U")).value(), (QStringList{QStringLiteral("app"), QStringLiteral("xy")}));
        // a quoted field code alone is no argument either (the specification removes it), but an explicit empty string stays
        QCOMPARE(parseExec(QStringLiteral("app \"%u\" x")).value(), (QStringList{QStringLiteral("app"), QStringLiteral("x")}));
        QCOMPARE(parseExec(QStringLiteral("app \"%f%F\"")).value(), (QStringList{QStringLiteral("app")}));
        QCOMPARE(parseExec(QStringLiteral("app \"\" x")).value(), (QStringList{QStringLiteral("app"), QString(), QStringLiteral("x")}));
        QCOMPARE(parseExec(QStringLiteral("app \"%%\"")).value(), (QStringList{QStringLiteral("app"), QStringLiteral("%")}));
    }

    void hostileExecLinesAreRefused()
    {
        for (const char *bad : {"", "   ", "%u", "app 'single quoted'", "app \"unterminated", "app $HOME", "app `id`", "app a;b", "app a|b", "app a&b", "app >out",
                                "app <in", "app ~", "app *", "app ?", "app #x", "app (x)", "app \\x", "app %z", "app %", "app \"bad \\q escape\"", "app \"end\\",
                                "app\ttab", "app\nline"}) {
            QVERIFY2(!parseExec(QString::fromLatin1(bad)), bad);
        }
        QVERIFY(!parseExec(QString(5000, QLatin1Char('a'))));
        QString many = QStringLiteral("app");
        for (int i = 0; i < 70; ++i) {
            many += QStringLiteral(" a");
        }
        QVERIFY(!parseExec(many));
        QVERIFY(!parseExec(QStringLiteral("\"\" x"))); // an empty program
    }

    // ---- planning and starting ----
    void gioIsPreferredThenGtkLaunchThenExec()
    {
        DesktopEntry entry;
        entry.id = QStringLiteral("org.example.app.desktop");
        entry.path = QStringLiteral("/usr/share/applications/org.example.app.desktop");
        entry.exec = QStringLiteral("example --new-window %U");
        QString why;
        auto plan = planLaunch(entry, finder({QStringLiteral("gio"), QStringLiteral("gtk-launch"), QStringLiteral("example")}), &why);
        QVERIFY(plan);
        QCOMPARE(plan->program, QStringLiteral("/usr/bin/gio"));
        QCOMPARE(plan->arguments, (QStringList{QStringLiteral("launch"), entry.path}));
        plan = planLaunch(entry, finder({QStringLiteral("gtk-launch"), QStringLiteral("example")}), &why);
        QVERIFY(plan);
        QCOMPARE(plan->program, QStringLiteral("/usr/bin/gtk-launch"));
        QCOMPARE(plan->arguments, (QStringList{QStringLiteral("--"), entry.id}));
        plan = planLaunch(entry, finder({QStringLiteral("example")}), &why);
        QVERIFY(plan);
        QCOMPARE(plan->how, QStringLiteral("exec"));
        QCOMPARE(plan->program, QStringLiteral("/usr/bin/example"));
        QCOMPARE(plan->arguments, (QStringList{QStringLiteral("--new-window")}));
    }

    void nothingStartsWhenNoSafeWayExists()
    {
        DesktopEntry entry;
        entry.id = QStringLiteral("a.desktop");
        entry.path = QStringLiteral("/usr/share/applications/a.desktop");
        entry.exec = QStringLiteral("example");
        QString why;
        QVERIFY(!planLaunch(entry, finder({}), &why)); // no gio, no gtk-launch, the program is not there
        QVERIFY(!why.isEmpty());
        entry.terminal = true;
        QVERIFY(!planLaunch(entry, finder({QStringLiteral("example")}), &why)); // a terminal program: not by the last resort
        entry.terminal = false;
        entry.exec = QStringLiteral("example $(id)");
        QVERIFY(!planLaunch(entry, finder({QStringLiteral("example")}), &why));
        entry.exec = QStringLiteral("example");
        entry.path.clear(); // an entry without a file
        QVERIFY(!planLaunch(entry, finder({QStringLiteral("gio")}), &why));
        entry.path = QStringLiteral("relative/path.desktop"); // not absolute
        QVERIFY(!planLaunch(entry, finder({QStringLiteral("gio")}), &why));
    }

    void launchStartsExactlyTheCommandPlanned()
    {
        DesktopEntry entry;
        entry.id = QStringLiteral("a.desktop");
        entry.path = QStringLiteral("/usr/share/applications/a.desktop");
        entry.exec = QStringLiteral("example");
        QList<LaunchCommand> started;
        QVERIFY(launch(entry, finder({QStringLiteral("gio")}), [&](const LaunchCommand &c) { started << c; return true; }));
        QCOMPARE(started.size(), qsizetype{1});
        QCOMPARE(started[0].arguments, (QStringList{QStringLiteral("launch"), entry.path}));
        QString why;
        QVERIFY(!launch(entry, finder({QStringLiteral("gio")}), [](const LaunchCommand &) { return false; }, &why));
        QCOMPARE(why, QStringLiteral("the program did not start"));
        QVERIFY(!launch(entry, finder({}), [&](const LaunchCommand &c) { started << c; return true; }));
        QCOMPARE(started.size(), qsizetype{1}); // nothing more
    }

    // ---- the catalogue model ----
    void theCatalogueListsFiltersAndResolves()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("applications"));
        testutil::writeFile(root + QStringLiteral("/b.desktop"), entryText({}, QStringLiteral("Bravo")));
        testutil::writeFile(root + QStringLiteral("/a.desktop"), entryText(QStringLiteral("Icon=alpha-icon\n"), QStringLiteral("Alpha")));
        testutil::writeFile(root + QStringLiteral("/c.desktop"), entryText({}, QStringLiteral("Charlie")));
        ShortcutCatalog catalog({root}, testEnv());
        catalog.refreshNow();
        QCOMPARE(catalog.rowCount(), 3);
        QCOMPARE(catalog.total(), 3);
        QCOMPARE(catalog.data(catalog.index(0), ShortcutCatalog::NameRole).toString(), QStringLiteral("Alpha"));
        QCOMPARE(catalog.data(catalog.index(0), ShortcutCatalog::IdRole).toString(), QStringLiteral("a.desktop"));
        QCOMPARE(catalog.nameFor(QStringLiteral("b.desktop")), QStringLiteral("Bravo"));
        QCOMPARE(catalog.nameFor(QStringLiteral("gone.desktop")), QString());
        catalog.setFilter(QStringLiteral("harl"));
        QCOMPARE(catalog.rowCount(), 1);
        QCOMPARE(catalog.data(catalog.index(0), ShortcutCatalog::NameRole).toString(), QStringLiteral("Charlie"));
        QCOMPARE(catalog.total(), 3);
        catalog.setFilter(QString(1000, QLatin1Char('z')));
        QCOMPARE(catalog.rowCount(), 0);
        QVERIFY(!catalog.data(catalog.index(5), ShortcutCatalog::NameRole).isValid());
        catalog.setFilter({});
        QCOMPARE(catalog.rowCount(), 3);
    }

    void theCatalogueScansOnAWorkerAndFollowsTheDirectory()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("applications"));
        testutil::writeFile(root + QStringLiteral("/a.desktop"), entryText({}, QStringLiteral("Alpha")));
        ShortcutCatalog catalog({root}, testEnv());
        QSignalSpy total(&catalog, &ShortcutCatalog::totalChanged);
        catalog.refresh();
        QTRY_COMPARE(catalog.total(), 1);
        testutil::writeFile(root + QStringLiteral("/b.desktop"), entryText({}, QStringLiteral("Bravo")));
        QTRY_COMPARE_WITH_TIMEOUT(catalog.total(), 2, 5000); // the directory watcher, debounced
    }

    // A trigger stores the window class an application's windows carry: StartupWMClass when the entry says it, else its id.
    void theCatalogueGivesTheWindowClassOfAnApplication()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("applications"));
        testutil::writeFile(root + QStringLiteral("/discord.desktop"), entryText(QStringLiteral("StartupWMClass=DiscordWin\n"), QStringLiteral("Discord")));
        testutil::writeFile(root + QStringLiteral("/org.example.Bravo.desktop"), entryText({}, QStringLiteral("Bravo")));
        testutil::writeFile(root + QStringLiteral("/odd.desktop"), entryText(QStringLiteral("StartupWMClass=bad\x01class\n"), QStringLiteral("Odd")));
        ShortcutCatalog catalog({root}, testEnv());
        catalog.refreshNow();
        QCOMPARE(catalog.rowCount(), 3);
        QCOMPARE(catalog.data(catalog.index(1), ShortcutCatalog::NameRole).toString(), QStringLiteral("Discord"));
        QCOMPARE(catalog.data(catalog.index(1), ShortcutCatalog::ClassRole).toString(), QStringLiteral("DiscordWin"));
        QCOMPARE(catalog.data(catalog.index(0), ShortcutCatalog::ClassRole).toString(), QStringLiteral("org.example.Bravo"));
        QCOMPARE(catalog.data(catalog.index(2), ShortcutCatalog::ClassRole).toString(), QStringLiteral("odd")); // a class with controls is no class
        QVERIFY(catalog.roleNames().values().contains("appClass"));

        // From a stored class back to the application, ignoring case as the trigger engine does.
        QCOMPARE(catalog.entryForClass(QStringLiteral("discordwin")).value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));
        QCOMPARE(catalog.entryForClass(QStringLiteral("ORG.EXAMPLE.BRAVO")).value(QStringLiteral("icon")).toString(), QStringLiteral("firefox"));
        QVERIFY(catalog.entryForClass(QStringLiteral("unknown")).isEmpty());
        QVERIFY(catalog.entryForClass(QString()).isEmpty());

        // Uninstalling takes it out of the list (the directory watcher, debounced).
        QFile::remove(root + QStringLiteral("/discord.desktop"));
        QTRY_COMPARE_WITH_TIMEOUT(catalog.total(), 2, 5000);
        QVERIFY(catalog.entryForClass(QStringLiteral("discordwin")).isEmpty());
    }

    // An entry without StartupWMClass is also known by the other names its windows commonly carry.
    void anEntryListsTheOtherNamesItsWindowsCarry()
    {
        const auto env = testEnv();
        const auto parse = [&](const char *exec, const QString &id) {
            return parseDesktopEntry(QByteArray("[Desktop Entry]\nType=Application\nName=A\nExec=") + exec + "\n", id, env);
        };
        const auto rdns = parse("/usr/bin/dolphin %U", QStringLiteral("org.kde.dolphin.desktop"));
        QVERIFY(rdns);
        QCOMPARE(rdns->windowClass, QStringLiteral("org.kde.dolphin")); // the stored format is unchanged
        QCOMPARE(rdns->classAliases, QStringList{QStringLiteral("dolphin")}); // the last part and the program are the same name
        const auto flatpak = parse("flatpak run --branch=stable --command=x com.example.App @@u %U @@", QStringLiteral("com.example.App.desktop"));
        QVERIFY(flatpak);
        QVERIFY(flatpak->classAliases.contains(QStringLiteral("App"), Qt::CaseInsensitive)); // (compared folded)
        const auto steam = parse("steam steam://rungameid/1245620", QStringLiteral("game.desktop"));
        QVERIFY(steam);
        QCOMPARE(steam->windowClass, QStringLiteral("steam_app_1245620")); // what the game's windows carry, so a trigger on it matches
        QVERIFY(steam->classAliases.contains(QStringLiteral("game")));
        QVERIFY(!steam->classAliases.contains(QStringLiteral("steam"))); // (Steam's own windows are not the game)
        const auto envd = parse("env FOO=1 /opt/x/bin/myapp --flag", QStringLiteral("myapp-launcher.desktop"));
        QVERIFY(envd);
        QVERIFY(envd->classAliases.contains(QStringLiteral("myapp")));
    }

    void theCatalogueFindsApplicationsByAnyOfTheirClasses()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("applications"));
        testutil::writeFile(root + QStringLiteral("/org.kde.dolphin.desktop"), QByteArray("[Desktop Entry]\nType=Application\nName=Files\nExec=dolphin %u\n"));
        testutil::writeFile(root + QStringLiteral("/wow.desktop"), QByteArray("[Desktop Entry]\nType=Application\nName=Wow\nExec=steam steam://rungameid/42\n"));
        ShortcutCatalog catalog({root}, testEnv());
        catalog.refreshNow();
        QCOMPARE(catalog.rowCount(), 2);
        catalog.setFilter(QStringLiteral("steam_app_42")); // not in the name, the id or the stored class
        QCOMPARE(catalog.rowCount(), 1);
        QCOMPARE(catalog.data(catalog.index(0), ShortcutCatalog::NameRole).toString(), QStringLiteral("Wow"));
        catalog.setFilter(QStringLiteral("KDE.DOL")); // part of the stored class
        QCOMPARE(catalog.rowCount(), 1);
        catalog.setFilter(QString());
        // A class as the compositor reports it resolves to the entry even when it is not the stored one.
        QCOMPARE(catalog.entryForClass(QStringLiteral("dolphin")).value(QStringLiteral("name")).toString(), QStringLiteral("Files"));
        QCOMPARE(catalog.entryForClass(QStringLiteral("steam_app_42")).value(QStringLiteral("name")).toString(), QStringLiteral("Wow"));
        QCOMPARE(catalog.entryForClass(QStringLiteral("Org.KDE.Dolphin.desktop")).value(QStringLiteral("name")).toString(), QStringLiteral("Files"));
        QVERIFY(catalog.entryForClass(QStringLiteral("steam_app_43")).isEmpty());
    }

    // ---- the property type ----
    void usershortcutIsAPropertyType()
    {
        using murasaki::WeProperty;
        QCOMPARE(murasaki::weTypeFromString(QStringLiteral("usershortcut")), WeProperty::Type::UserShortcut);
        QCOMPARE(murasaki::weTypeToString(WeProperty::Type::UserShortcut), QStringLiteral("usershortcut"));
    }
};

QTEST_MAIN(TstShortcut)
#include "tst_shortcut.moc"
