// The automation's lists are closed (docs/pets/automation.md): what a trigger can do, what can set it off, and what the code that runs it is allowed to touch. A change
// to any of them has to be made on purpose, here, in front of a reviewer.

#include "automation.h"
#include "automationmanager.h"
#include "timeline.h"
#include "trigger.h"

#include <QFile>
#include <QRegularExpression>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QString read(const QString &relative)
{
    QFile f(QStringLiteral(SOURCE_DIR) + u'/' + relative);
    if (!f.open(QIODevice::ReadOnly))
        qFatal("cannot read %s", qPrintable(relative));
    return QString::fromUtf8(f.readAll());
}

}

class TstAutomationClosed : public QObject
{
    Q_OBJECT

private slots:
    void theActionsOfATriggerAreExactlyTheseNine()
    {
        QStringList names;
        for (const TriggerActionType type : allTriggerActionTypes())
            names << triggerActionName(type);
        QCOMPARE(names, (QStringList{u"timeline"_s, u"scene"_s, u"profile"_s, u"hide-all"_s, u"show-all"_s, u"toggle-hidden"_s, u"stop-sounds"_s, u"mute"_s, u"play-sound"_s}));
        for (const TriggerActionType type : allTriggerActionTypes())
            QCOMPARE(triggerActionFromName(triggerActionName(type)), std::optional<TriggerActionType>(type));
        for (const QString &bad : {u"run"_s, u"exec"_s, u"shell"_s, u"open"_s, u"open-url"_s, u"command"_s, u"notify"_s, u"http"_s, u"file"_s, u""_s})
            QVERIFY2(!triggerActionFromName(bad), qPrintable(bad));
    }
    void theSourcesAreExactlyTheseFifteenAndNoneReadsTheScreenOrTheKeyboard()
    {
        QStringList names;
        for (const TriggerKind kind : allTriggerKinds())
            names << triggerKindName(kind);
        QCOMPARE(names, (QStringList{u"app-focus"_s, u"app-open"_s, u"fullscreen"_s, u"time"_s, u"interval"_s, u"idle"_s, u"power"_s, u"session"_s, u"game"_s, u"output"_s,
                                     u"shortcut"_s, u"signal-file"_s, u"fire"_s, u"dbus"_s, u"screen"_s}));
        for (const QString &bad : {u"screen-region"_s, u"keyboard"_s, u"key"_s, u"mouse"_s, u"gamepad"_s, u"microphone"_s, u"network"_s, u"http"_s, u"process"_s, u"file-content"_s})
            QVERIFY2(!triggerKindFromName(bad), qPrintable(bad));
    }
    void theActionsOfATimelineAreExactlyTheseEight()
    {
        QStringList names;
        for (const ActionKind kind : allActionKinds())
            names << actionKindName(kind);
        QCOMPARE(names, (QStringList{u"place"_s, u"show"_s, u"hide"_s, u"move"_s, u"sound"_s, u"wait"_s, u"stopSounds"_s, u"track"_s}));
    }
    void whatTheManagerCanCallIsAListOfPetThings()
    {
        // The hooks are everything the automation may do to the world. This reads the struct in the header and compares its members.
        const QString header = read(u"src/daemon/automationmanager.h"_s);
        const int begin = header.indexOf(u"struct AutomationHooks {"_s);
        const int end = header.indexOf(u"};"_s, begin);
        QVERIFY(begin >= 0 && end > begin);
        static const QRegularExpression member(u"std::function<[^;]*?>\\s+(\\w+);|ProfileItemCheck\\s+(\\w+);"_s);
        QStringList members;
        auto it = member.globalMatch(header.mid(begin, end - begin));
        while (it.hasNext()) {
            const auto m = it.next();
            members << (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1));
        }
        QCOMPARE(members, (QStringList{u"setPetsHidden"_s, u"petsHidden"_s, u"playSound"_s, u"stopSounds"_s, u"startScene"_s, u"setMuted"_s, u"muted"_s, u"checkItem"_s, u"petSize"_s, u"nowMs"_s, u"nowLocal"_s}));
    }
    void theAutomationCodeNeverStartsAProgramOrOpensAFileOrAnAddress()
    {
        const QStringList files = {u"src/core/automation.h"_s,       u"src/core/automation.cpp"_s,      u"src/core/automationsettings.h"_s, u"src/core/automationsettings.cpp"_s,
                                   u"src/core/automationstore.h"_s,  u"src/core/petruntime.h"_s,        u"src/core/petruntime.cpp"_s,       u"src/core/packautomation.h"_s, u"src/core/packautomation.cpp"_s, u"src/import/triggerspack.h"_s, u"src/import/triggerspack.cpp"_s, u"src/core/profile.h"_s,
                                   u"src/core/profile.cpp"_s,        u"src/core/profileswitch.h"_s,     u"src/core/profileswitch.cpp"_s,    u"src/core/timeline.h"_s,
                                   u"src/core/timeline.cpp"_s,       u"src/core/timelinerunner.h"_s,    u"src/core/timelinerunner.cpp"_s,   u"src/core/trigger.h"_s,
                                   u"src/core/trigger.cpp"_s,        u"src/core/triggerengine.h"_s,     u"src/core/triggerengine.cpp"_s,    u"src/daemon/automationmanager.h"_s,
                                   u"src/daemon/automationmanager.cpp"_s, u"src/daemon/automationsources.h"_s, u"src/daemon/automationsources.cpp"_s,
                                   u"src/daemon/murasakiserviceautomation.cpp"_s, u"src/overlay/triggershortcuts.h"_s, u"src/overlay/triggershortcuts.cpp"_s};
        static const QRegularExpression forbidden(
            u"\\b(QProcess|QDesktopServices|QUrl|QNetworkAccessManager|QTcpSocket|QLocalSocket|QWebEngine|popen|execv|execve|execl|execlp|execvp|posix_spawn|for[k]|vfor[k]|dlopen|QLibrary|QPluginLoader|QScreen|grabWindow|XGrabKey|QClipboard)\\b"_s);
        for (const QString &file : files) {
            const QString text = read(file);
            const auto match = forbidden.match(text);
            QVERIFY2(!match.hasMatch(), qPrintable(file + u": "_s + match.captured()));
            QVERIFY2(!text.contains(QRegularExpression(u"\\bsystem\\s*\\("_s)), qPrintable(file + u": system()"_s));
        }
        // the one place a file's contents could be read: the signal files are listed by name and time only
        const QString sources = read(u"src/daemon/automationsources.cpp"_s);
        QVERIFY(!sources.contains(u"readAll"_s) && !sources.contains(u"QTextStream"_s) && !sources.contains(u"open(QIODevice"_s));
        // and the daemon's hooks, as the service builds them, are these pet things only
        const QString service = read(u"src/daemon/murasakiserviceautomation.cpp"_s);
        QVERIFY(service.contains(u"callOverlay(u\"PlayPetSound\"_s"_s) && service.contains(u"callOverlay(u\"StopOneShotPetSounds\"_s"_s));
        QCOMPARE(service.count(u"callOverlay("_s), 3); // the definition and the two calls
    }
    void theNamesOfTheDocumentAreTheNamesOfTheCode()
    {
        const QString doc = read(u"docs/pets/automation.md"_s);
        for (const QString &name : automationNames())
            QVERIFY2(doc.contains(u'`' + name + u'`'), qPrintable(name));
        // every name the document writes in backticks that looks like one is in the list
        static const QRegularExpression written(u"`((?:profile|trigger|timeline|automation): [a-z ]+)`"_s);
        auto it = written.globalMatch(doc);
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            QVERIFY2(automationNames().contains(name), qPrintable(name));
        }
        QCOMPARE(automationNames().size(), 14);
    }
    void everyActionAndSourceIsInTheDocument()
    {
        const QString doc = read(u"docs/pets/automation.md"_s);
        for (const TriggerActionType type : allTriggerActionTypes())
            QVERIFY2(doc.contains(u'`' + triggerActionName(type) + u'`'), qPrintable(triggerActionName(type)));
        for (const TriggerKind kind : allTriggerKinds())
            QVERIFY2(doc.contains(u'`' + triggerKindName(kind) + u'`'), qPrintable(triggerKindName(kind)));
        for (const ActionKind kind : allActionKinds())
            QVERIFY2(doc.contains(u'`' + actionKindName(kind) + u'`'), qPrintable(actionKindName(kind)));
    }
};

QTEST_GUILESS_MAIN(TstAutomationClosed)
#include "tst_automationclosed.moc"
