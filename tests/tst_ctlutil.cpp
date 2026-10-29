#include "ctlutil.h"
#include "testutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestCtlUtil : public QObject
{
    Q_OBJECT

private slots:
    void resolvesDesktopArguments();
    void detectsMatchingDesktopArguments();
    void findsAssignmentsByExactKey();
    void keepsOptionsWhenMuting();
    void keepsSpanWhenMuting();
    void formatsOutputLines();
    void parsesNumberArguments();
    void buildsPetAddOptions();
    void formatsPetLines();
    void formatsPuppetItemAndWindowLines();
    void buildsPetStageChanges();
    void buildsPetStateAndTrackChanges();
    void buildsPetTimingChanges();
    void buildsPetAudioChanges();
    void buildsPetBehaviourChanges();
    void choosesArrangeTarget();
    void resolvesPlaylistArguments();
    void formatsPlaylistLines();
    void buildsPlaylistChanges();
    void findsPlaylistTargets();
    void formatsSteamCmdStatus();
    void readsTheAppArgument();
    void buildsTheImportSpec();
    void refusesBadImportOptions();
    void buildsTheVideoSpec();
    void refusesTheRest();
    void resolvesNamedArguments();
    void formatsAutomationLines();
    void buildsTriggerChanges();
    void controlCharactersNeverReachTheTerminal();
};

void TestCtlUtil::resolvesDesktopArguments()
{
    const QJsonArray desktops = QJsonDocument::fromJson(
        "[{\"id\": \"a1b2\", \"name\": \"Work\", \"number\": 1}, {\"id\": \"c3d4\", \"name\": \"Games\", \"number\": 2}]")
                                    .array();
    QCOMPARE(resolveDesktopArg(u"c3d4"_s, desktops), u"c3d4"_s);
    QCOMPARE(resolveDesktopArg(u"2"_s, desktops), u"c3d4"_s);
    QCOMPARE(resolveDesktopArg(u"Work"_s, desktops), u"a1b2"_s);
    QCOMPARE(resolveDesktopArg(u"games"_s, desktops), u"c3d4"_s);
    QCOMPARE(resolveDesktopArg(u"9"_s, desktops), u"9"_s);         // unknown: kept as an id
    QCOMPARE(resolveDesktopArg(u"Music"_s, desktops), u"Music"_s);
    QCOMPARE(resolveDesktopArg(u"*"_s, desktops), u"*"_s);
    QCOMPARE(resolveDesktopArg(u"2"_s, QJsonArray()), u"2"_s);
}

void TestCtlUtil::detectsMatchingDesktopArguments()
{
    const QJsonArray desktops = QJsonDocument::fromJson(
        "[{\"id\": \"a1b2\", \"name\": \"Work\", \"number\": 1}, {\"id\": \"c3d4\", \"name\": \"Games\", \"number\": 2}]")
                                    .array();
    QVERIFY(desktopArgMatches(u"a1b2"_s, desktops));
    QVERIFY(desktopArgMatches(u"2"_s, desktops));
    QVERIFY(desktopArgMatches(u"games"_s, desktops));
    QVERIFY(!desktopArgMatches(u"Music"_s, desktops));
    QVERIFY(!desktopArgMatches(u"9"_s, desktops));
    QVERIFY(!desktopArgMatches(u"Work"_s, QJsonArray()));
}

void TestCtlUtil::findsAssignmentsByExactKey()
{
    const QJsonArray all = QJsonDocument::fromJson(
        "[{\"output\": \"*\", \"desktop\": \"*\", \"itemId\": \"we:1\"},"
        " {\"output\": \"DP-1\", \"desktop\": \"*\", \"itemId\": \"we:2\"}]").array();
    QCOMPARE(findAssignment(all, u"DP-1"_s, u"*"_s).value(u"itemId"_s).toString(), u"we:2"_s);
    QVERIFY(findAssignment(all, u"DP-2"_s, u"*"_s).isEmpty());
}

void TestCtlUtil::keepsOptionsWhenMuting()
{
    const QJsonObject entry = testutil::json(
        "{\"output\": \"*\", \"desktop\": \"*\", \"itemId\": \"we:1\", \"muted\": false, \"fill\": \"fit\","
        " \"properties\": {\"k\": 1}}");
    const QJsonObject options = optionsWithMuted(entry, true);
    QCOMPARE(options.value(u"muted"_s).toBool(), true);
    QCOMPARE(options.value(u"fill"_s).toString(), u"fit"_s);
    QCOMPARE(options.value(u"properties"_s).toObject().value(u"k"_s).toInt(), 1);
    QVERIFY(!options.contains(u"itemId"_s));
}

void TestCtlUtil::keepsSpanWhenMuting()
{
    const QJsonObject spanned = testutil::json(
        "{\"output\": \"*\", \"desktop\": \"*\", \"itemId\": \"we:1\", \"muted\": false, \"fill\": \"fit\","
        " \"span\": true, \"properties\": {}}");
    QCOMPARE(optionsWithMuted(spanned, true).value(u"span"_s).toBool(), true);
    // an assignment without it stays without it
    const QJsonObject plain = testutil::json("{\"output\": \"*\", \"desktop\": \"*\", \"itemId\": \"we:1\"}");
    QVERIFY(!optionsWithMuted(plain, true).contains(u"span"_s));
}

void TestCtlUtil::formatsOutputLines()
{
    QCOMPARE(formatOutputLine(testutil::json(
                 "{\"name\": \"DP-1\", \"desktop\": \"d1\", \"itemId\": \"we:1\", \"occluded\": false, \"playing\": true}")),
             u"DP-1\td1\twe:1\tplaying"_s);
    QCOMPARE(formatOutputLine(testutil::json(
                 "{\"name\": \"DP-1\", \"desktop\": \"d1\", \"itemId\": \"we:1\", \"occluded\": true, \"playing\": false}")),
             u"DP-1\td1\twe:1\tpaused (covered)"_s);
    QCOMPARE(formatOutputLine(testutil::json(
                 "{\"name\": \"DP-1\", \"desktop\": \"*\", \"itemId\": \"\", \"occluded\": false, \"playing\": false}")),
             u"DP-1\t*\t-\tidle"_s);
}

void TestCtlUtil::parsesNumberArguments()
{
    double v = 0;
    QVERIFY(parseNumberArg(u"12"_s, &v));
    QCOMPARE(v, 12.0);
    QVERIFY(parseNumberArg(u" -3.5 "_s, &v));
    QCOMPARE(v, -3.5);
    for (const QString &bad : {QString(), u"abc"_s, u"12px"_s, u"inf"_s, u"nan"_s, u"1e999"_s})
        QVERIFY2(!parseNumberArg(bad, &v), qPrintable(bad));
}

void TestCtlUtil::buildsPetAddOptions()
{
    QJsonObject options;
    QString error;
    QVERIFY(petAddOptions(QString(), QString(), QString(), QString(), &options, &error));
    QVERIFY(options.isEmpty());
    QVERIFY(petAddOptions(u"10"_s, QString(), u"300.5"_s, QString(), &options, &error));
    QCOMPARE(options, (QJsonObject{{u"x"_s, 10}, {u"width"_s, 300.5}}));
    QJsonObject withFps;
    QVERIFY(petAddOptions(QString(), QString(), QString(), u"24"_s, &withFps, &error));
    QCOMPARE(withFps, (QJsonObject{{u"fps"_s, 24}}));
    QVERIFY(petAddOptions(QString(), QString(), QString(), u"0"_s, &withFps, &error));
    QCOMPARE(withFps.value(u"fps"_s).toInt(-1), 0);
    QJsonObject untouched;
    QVERIFY(!petAddOptions(QString(), QString(), QString(), u"fast"_s, &untouched, &error));
    QVERIFY(error.contains(u"--fps"_s));
    QVERIFY(!petAddOptions(u"10"_s, u"left"_s, QString(), QString(), &untouched, &error));
    QVERIFY(error.contains(u"--y"_s));
    QVERIFY(error.contains(u"left"_s));
}

void TestCtlUtil::buildsPetAudioChanges()
{
    QJsonObject changes;
    QString error;
    QVERIFY(petAudioChanges({}, &changes, &error));
    QVERIFY(changes.isEmpty());
    PetAudioArgs all;
    all.asset = QString(64, QLatin1Char('a'));
    all.mode = u"animation_loop"_s;
    all.volume = u"40"_s;
    all.offset = u"-250"_s;
    QVERIFY(petAudioChanges(all, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"audio"_s, QJsonObject{{u"asset"_s, QString(64, QLatin1Char('a'))}, {u"mode"_s, u"animation_loop"_s}, {u"volume"_s, 40}, {u"offsetMs"_s, -250}}}}));
    PetAudioArgs one;
    one.volume = u"0"_s;
    QVERIFY(petAudioChanges(one, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"audio"_s, QJsonObject{{u"volume"_s, 0}}}}));
    for (const auto &bad : {std::pair{&PetAudioArgs::asset, u"abc"_s}, std::pair{&PetAudioArgs::mode, u"loud"_s}, std::pair{&PetAudioArgs::volume, u"7.5"_s},
                            std::pair{&PetAudioArgs::volume, u""_s}, std::pair{&PetAudioArgs::offset, u"soon"_s}, std::pair{&PetAudioArgs::offset, u" 5"_s}}) {
        PetAudioArgs args;
        args.*(bad.first) = bad.second;
        QVERIFY2(!petAudioChanges(args, &changes, &error), qPrintable(bad.second));
        QVERIFY(error.startsWith(u"--"_s));
    }
    QCOMPARE(formatPetAudioAssetLine({{u"asset"_s, u"ab"_s}, {u"name"_s, u"song.mp3"_s}, {u"durationMs"_s, 1500}, {u"refs"_s, 2}}), u"ab\tsong.mp3\t1500\t2"_s);
}

void TestCtlUtil::buildsPetBehaviourChanges()
{
    QJsonObject changes;
    QString error;
    QVERIFY(petBehaviourChanges({}, &changes, &error));
    QVERIFY(changes.isEmpty()); // nothing asked: the caller says so
    PetBehaviourArgs all;
    all.clickThrough = u"off"_s;
    all.lock = u"on"_s;
    all.keepAbove = u"off"_s;
    all.hoverHide = u"on"_s;
    all.focusFade = u"off"_s;
    all.smartDodge = u"on"_s;
    all.rotation = u"-90.5"_s;
    all.flipH = u"on"_s;
    all.flipV = u"off"_s;
    QVERIFY(petBehaviourChanges(all, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"clickThrough"_s, false}, {u"locked"_s, true}, {u"keepAbove"_s, false}, {u"hoverHide"_s, true},
                                   {u"focusFade"_s, false}, {u"smartDodge"_s, true}, {u"rotation"_s, -90.5}, {u"flipH"_s, true}, {u"flipV"_s, false}}));
    PetBehaviourArgs one;
    one.lock = u"on"_s;
    QVERIFY(petBehaviourChanges(one, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"locked"_s, true}})); // only what was given
    // anything but on/off for a switch, and a non-number angle, name the option
    struct Bad {
        std::optional<QString> PetBehaviourArgs::*field;
        QString option;
    };
    for (const Bad &bad : {Bad{&PetBehaviourArgs::clickThrough, u"--click-through"_s}, Bad{&PetBehaviourArgs::lock, u"--lock"_s},
                           Bad{&PetBehaviourArgs::keepAbove, u"--keep-above"_s}, Bad{&PetBehaviourArgs::hoverHide, u"--hover-hide"_s},
                           Bad{&PetBehaviourArgs::focusFade, u"--focus-fade"_s}, Bad{&PetBehaviourArgs::smartDodge, u"--smart-dodge"_s},
                           Bad{&PetBehaviourArgs::flipH, u"--flip-h"_s}, Bad{&PetBehaviourArgs::flipV, u"--flip-v"_s}}) {
        for (const QString &value : {u"true"_s, u"1"_s, u"ON"_s, u"yes"_s, QString(u""_s)}) { // (an empty value is given and wrong)
            PetBehaviourArgs args;
            args.*(bad.field) = value;
            QJsonObject untouched;
            QVERIFY2(!petBehaviourChanges(args, &untouched, &error), qPrintable(bad.option + value));
            QVERIFY2(error.contains(bad.option), qPrintable(error));
            QVERIFY(untouched.isEmpty());
        }
    }
    for (const QString &value : {u"fast"_s, u"nan"_s, u"inf"_s, u"90deg"_s, QString(u""_s)}) {
        PetBehaviourArgs args;
        args.rotation = value;
        QVERIFY2(!petBehaviourChanges(args, &changes, &error), qPrintable(value));
        QVERIFY(error.contains(u"--rotation"_s));
    }
}

void TestCtlUtil::buildsPetTimingChanges()
{
    QJsonObject changes;
    QString error;
    QVERIFY(petTimingChanges({u"file"_s}, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"fps"_s, 0}, {u"speed"_s, 1}}));
    QVERIFY(petTimingChanges({u"fixed"_s}, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"fps"_s, 24}, {u"speed"_s, 1}})); // the default rate when none is given
    QVERIFY(petTimingChanges({u"fixed"_s, u"24"_s}, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"fps"_s, 24}, {u"speed"_s, 1}}));
    QVERIFY(petTimingChanges({u"speed"_s, u"0.5"_s}, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"fps"_s, 0}, {u"speed"_s, 0.5}}));
    for (const QStringList &bad : {QStringList{}, QStringList{u"fast"_s}, QStringList{u"file"_s, u"3"_s}, QStringList{u"speed"_s},
                                   QStringList{u"speed"_s, u"quick"_s}, QStringList{u"fixed"_s, u"x"_s}, QStringList{u"fixed"_s, u"1"_s, u"2"_s}}) {
        QJsonObject untouched;
        QVERIFY2(!petTimingChanges(bad, &untouched, &error), qPrintable(bad.join(u' ')));
        QVERIFY(untouched.isEmpty());
        QVERIFY(!error.isEmpty());
    }
}

void TestCtlUtil::formatsPetLines()
{
    const QJsonObject pet = QJsonDocument::fromJson(
        "{\"petId\": \"p1\", \"itemId\": \"anima:9\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 10, \"y\": 20.5, "
        "\"width\": 300, \"opacity\": 0.5, \"fps\": 30, \"paused\": false}").object();
    QCOMPARE(formatPetLine(pet), u"p1\tanima:9\tidle\tDP-1\t10\t20.5\t300\t0.5\t30\tplaying\t1"_s); // (the speed comes last, so that the columns scripts read stay where they were)
    QJsonObject paused = pet;
    paused.insert(u"paused"_s, true);
    QVERIFY(formatPetLine(paused).endsWith(u"\tpaused\t1"_s));
    QJsonObject fast = pet;
    fast.insert(u"fps"_s, 0);
    fast.insert(u"speed"_s, 1.5);
    QVERIFY(formatPetLine(fast).endsWith(u"\t0\tplaying\t1.5"_s));
}

void TestCtlUtil::formatsPuppetItemAndWindowLines()
{
    // an item that is not a puppet has no line
    QVERIFY(formatPuppetItemLine(QJsonObject{{u"id"_s, u"anima:1"_s}, {u"kind"_s, u"pet"_s}}).isEmpty());
    QJsonObject item{{u"id"_s, u"local:puppet:abc"_s}, {u"status"_s, u"ok"_s}, {u"title"_s, u"Haru"_s}, {u"format"_s, u"puppet"_s},
                     {u"puppet"_s, QJsonObject{{u"backend"_s, u"open"_s}, {u"checked"_s, true}, {u"canvas"_s, QJsonArray{800, 1200}},
                                               {u"motionGroups"_s, QJsonArray{QJsonObject{{u"name"_s, u"Idle"_s}}}}, {u"expressions"_s, QJsonArray{u"smile"_s, u"sad"_s}},
                                               {u"reasons"_s, QJsonArray()}, {u"degraded"_s, QJsonArray{u"puppet: physics rig skipped: p.json"_s}}}}};
    QCOMPARE(formatPuppetItemLine(item), u"local:puppet:abc\tok\topen\tHaru\tchecked\t800x1200\tgroups:1\texpressions:2\tpuppet: physics rig skipped: p.json"_s);
    // unchecked, no canvas, a reason wins over a degraded name, and the status detail is the last resort
    QJsonObject bare{{u"id"_s, u"local:puppet:x"_s}, {u"status"_s, u"Invalid"_s}, {u"statusDetail"_s, u"puppet: not a puppet"_s}, {u"format"_s, u"puppet"_s}};
    QCOMPARE(formatPuppetItemLine(bare), u"local:puppet:x\tInvalid\t\t\tunchecked\t-\tgroups:0\texpressions:0\tpuppet: not a puppet"_s);
    QJsonObject puppet = item.value(u"puppet"_s).toObject();
    puppet[u"reasons"_s] = QJsonArray{u"puppet: too large: kMaxVertices"_s};
    item[u"puppet"_s] = puppet;
    QVERIFY(formatPuppetItemLine(item).endsWith(u"puppet: too large: kMaxVertices"_s));

    QVERIFY(formatPuppetWindowLine(QJsonObject{{u"petId"_s, u"p1"_s}}).isEmpty());
    const QJsonObject window{{u"petId"_s, u"p1"_s}, {u"degraded"_s, QJsonArray{u"puppet: engine restarted"_s, u"hit mask: coarse"_s}},
                             {u"puppet"_s, QJsonObject{{u"state"_s, u"restarting"_s}, {u"fps"_s, 30}, {u"crashes"_s, 1}, {u"group"_s, u"Idle"_s}, {u"expression"_s, u"smile"_s}, {u"look"_s, true}}}};
    QCOMPARE(formatPuppetWindowLine(window), u"p1\trestarting\t30fps\tcrashes:1\tgroup:Idle\texpression:smile\tlook\tpuppet: engine restarted, hit mask: coarse"_s);

    // names come from files: no escape, newline or tab gets through to the terminal or splits a line
    QJsonObject hostile = item;
    hostile[u"title"_s] = u"a\x1b[2Jb\tc\n"_s;
    const QString line = formatPuppetItemLine(hostile);
    QVERIFY(!line.contains(QChar(0x1b)) && !line.contains(u'\n'));
    QCOMPARE(line.count(u'\t'), 8);
    QVERIFY(line.contains(u"a?[2Jb?c?"_s));
    QJsonObject hostileWindow = window;
    hostileWindow[u"petId"_s] = u"p\x1b]0;x\x07"_s;
    QVERIFY(!formatPuppetWindowLine(hostileWindow).contains(QChar(0x1b)));
}

void TestCtlUtil::buildsPetStageChanges()
{
    QJsonObject changes;
    QString error;
    QVERIFY(petStageChanges(u"output"_s, std::nullopt, std::nullopt, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"stage"_s, QJsonObject{{u"mode"_s, u"output"_s}}}})); // only what was said: the rest keeps its value
    QVERIFY(petStageChanges(u"desktop"_s, QString(u"cover"_s), QString(u"hide"_s), &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"stage"_s, QJsonObject{{u"mode"_s, u"desktop"_s}, {u"fit"_s, u"cover"_s}, {u"overFullscreen"_s, u"hide"_s}}}}));
    QVERIFY(petStageChanges(u"off"_s, std::nullopt, std::nullopt, &changes, &error));
    QCOMPARE(changes.value(u"stage"_s).toObject().value(u"mode"_s).toString(), u"off"_s);
    // a word the daemon would refuse is refused here, naming the option
    for (const QString &mode : {QString(), u"full"_s, u"Output"_s, u"on"_s, u"1"_s}) {
        QJsonObject untouched;
        QVERIFY2(!petStageChanges(mode, std::nullopt, std::nullopt, &untouched, &error), qPrintable(mode));
        QVERIFY(untouched.isEmpty());
        QVERIFY(error.contains(u"stage mode"_s));
    }
    QVERIFY(!petStageChanges(u"output"_s, QString(u"fill"_s), std::nullopt, &changes, &error));
    QVERIFY(error.contains(u"--fit"_s));
    QVERIFY(!petStageChanges(u"output"_s, QString(), std::nullopt, &changes, &error)); // an empty value is given and wrong
    QVERIFY(!petStageChanges(u"output"_s, std::nullopt, QString(u"always"_s), &changes, &error));
    QVERIFY(error.contains(u"--over-fullscreen"_s));
    // the pet list names the mode in a last column, only for a pet that is a stage
    const QJsonObject pet = QJsonDocument::fromJson("{\"petId\": \"p1\", \"itemId\": \"anima:9\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 10, \"y\": 20.5, "
                                                    "\"width\": 300, \"opacity\": 0.5, \"fps\": 30, \"paused\": false, \"stage\": {\"mode\": \"desktop\"}}").object();
    QVERIFY(formatPetLine(pet).endsWith(u"\tplaying\t1\tdesktop"_s));
    QJsonObject off = pet;
    off.insert(u"stage"_s, QJsonObject{{u"mode"_s, u"off"_s}});
    QVERIFY(formatPetLine(off).endsWith(u"\tplaying\t1"_s));
}

void TestCtlUtil::buildsPetStateAndTrackChanges()
{
    QJsonObject changes;
    QString error;
    QVERIFY(petStateChanges({u"grabbed"_s, u"variant"_s, u"hold"_s}, std::nullopt, std::nullopt, false, std::nullopt, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"states"_s, QJsonObject{{u"grabbed"_s, QJsonObject{{u"variant"_s, u"hold"_s}}}}}}));
    QVERIFY(petStateChanges({u"idle"_s, u"sequence"_s, u"a, b,c"_s}, std::nullopt, std::nullopt, false, std::nullopt, &changes, &error));
    QCOMPARE(changes.value(u"states"_s).toObject().value(u"idle"_s).toObject().value(u"sequence"_s).toObject(),
             (QJsonObject{{u"items"_s, QJsonArray{u"a"_s, u"b"_s, u"c"_s}}})); // only what was said
    QVERIFY(petStateChanges({u"released"_s, u"sequence"_s, u"x"_s}, QString(u"random"_s), QString(u"click"_s), true, QString(u"2000"_s), &changes, &error));
    QCOMPARE(changes.value(u"states"_s).toObject().value(u"released"_s).toObject().value(u"sequence"_s).toObject(),
             (QJsonObject{{u"items"_s, QJsonArray{u"x"_s}}, {u"order"_s, u"random"_s}, {u"advance"_s, u"click"_s}, {u"once"_s, true}, {u"holdMs"_s, 2000}}));
    QVERIFY(petStateChanges({u"clear"_s}, std::nullopt, std::nullopt, false, std::nullopt, &changes, &error));
    QVERIFY(changes.value(u"states"_s).isNull());
    QVERIFY(petStateChanges({u"clear"_s, u"moving"_s}, std::nullopt, std::nullopt, false, std::nullopt, &changes, &error));
    QVERIFY(changes.value(u"states"_s).toObject().value(u"moving"_s).isNull());
    const auto bad = [&](const QStringList &words, std::optional<QString> order = {}, std::optional<QString> advance = {}, bool once = false, std::optional<QString> hold = {}) {
        QJsonObject untouched;
        error.clear();
        const bool ok = petStateChanges(words, order, advance, once, hold, &untouched, &error);
        return !ok && untouched.isEmpty() && !error.isEmpty();
    };
    QVERIFY(bad({}));
    QVERIFY(bad({u"idle"_s}));
    QVERIFY(bad({u"static"_s, u"variant"_s, u"a"_s}));
    QVERIFY(bad({u"idle"_s, u"variant"_s}));
    QVERIFY(bad({u"idle"_s, u"variant"_s, u"a"_s}, QString(u"random"_s)));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, u"a,,b"_s}));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, QString()}));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, u"a"_s}, QString(u"sideways"_s)));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, u"a"_s}, {}, QString(u"never"_s)));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, u"a"_s}, {}, {}, false, QString(u"soon"_s)));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, u"a"_s}, {}, {}, false, QString(u"100"_s)));
    QVERIFY(bad({u"idle"_s, u"sequence"_s, QStringList(QList<QString>(17, u"v"_s)).join(u',')}));
    QVERIFY(bad({u"clear"_s, u"nap"_s}));
    QVERIFY(bad({u"clear"_s}, QString(u"random"_s))); // options for a sequence mean nothing to clear
    QVERIFY(bad({u"clear"_s, u"idle"_s}, {}, {}, true));
    QVERIFY(bad({u"clear"_s, u"idle"_s, u"moving"_s}));

    QVERIFY(petTrackChanges(u"look"_s, std::nullopt, std::nullopt, false, false, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}}}}));
    QVERIFY(petTrackChanges(u"follow"_s, QString(u"250"_s), QString(u"1.5"_s), false, true, &changes, &error));
    QCOMPARE(changes, (QJsonObject{{u"tracking"_s, QJsonObject{{u"mode"_s, u"follow"_s}, {u"distance"_s, 250}, {u"speed"_s, 1.5}, {u"returnHome"_s, false}}}}));
    QVERIFY(petTrackChanges(u"follow"_s, std::nullopt, std::nullopt, true, false, &changes, &error));
    QCOMPARE(changes.value(u"tracking"_s).toObject().value(u"returnHome"_s).toBool(), true);
    for (const QString &mode : {QString(), u"chase"_s, u"Look"_s}) {
        QJsonObject untouched;
        QVERIFY2(!petTrackChanges(mode, std::nullopt, std::nullopt, false, false, &untouched, &error), qPrintable(mode));
        QVERIFY(untouched.isEmpty() && error.contains(u"tracking mode"_s));
    }
    QVERIFY(!petTrackChanges(u"follow"_s, QString(u"far"_s), std::nullopt, false, false, &changes, &error) && error.contains(u"--distance"_s));
    QVERIFY(!petTrackChanges(u"follow"_s, QString(u"-1"_s), std::nullopt, false, false, &changes, &error));
    QVERIFY(!petTrackChanges(u"follow"_s, QString(u"4001"_s), std::nullopt, false, false, &changes, &error));
    QVERIFY(!petTrackChanges(u"follow"_s, std::nullopt, QString(u"fast"_s), false, false, &changes, &error) && error.contains(u"--speed"_s));
    QVERIFY(!petTrackChanges(u"follow"_s, std::nullopt, QString(u"9"_s), false, false, &changes, &error));
    QVERIFY(!petTrackChanges(u"follow"_s, std::nullopt, std::nullopt, true, true, &changes, &error));
    // the pet list names tracking and states in last columns, only for a pet that has them
    QJsonObject pet = QJsonDocument::fromJson("{\"petId\": \"p1\", \"itemId\": \"anima:9\", \"variant\": \"idle\", \"output\": \"DP-1\", \"x\": 10, \"y\": 20.5, "
                                              "\"width\": 300, \"opacity\": 0.5, \"fps\": 30, \"paused\": false}").object();
    const QString plain = formatPetLine(pet);
    pet.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}, {u"distance"_s, 300}});
    QCOMPARE(formatPetLine(pet), plain); // off is nothing to list
    pet.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"follow"_s}});
    QCOMPARE(formatPetLine(pet), plain + u"\ttracking:follow"_s);
    pet.insert(u"states"_s, QJsonObject{{u"released"_s, QJsonObject{{u"variant"_s, u"r"_s}}}, {u"idle"_s, QJsonObject{{u"variant"_s, u"i"_s}}}});
    QCOMPARE(formatPetLine(pet), plain + u"\ttracking:follow\tstates:idle,released"_s);
}

void TestCtlUtil::choosesArrangeTarget()
{
    QCOMPARE(arrangeTarget(u"on"_s, false), std::optional<bool>(true));
    QCOMPARE(arrangeTarget(u"on"_s, true), std::optional<bool>(true));
    QCOMPARE(arrangeTarget(u"off"_s, true), std::optional<bool>(false));
    QCOMPARE(arrangeTarget(u"toggle"_s, false), std::optional<bool>(true));
    QCOMPARE(arrangeTarget(u"toggle"_s, true), std::optional<bool>(false));
    QVERIFY(!arrangeTarget(u"maybe"_s, false));
    QVERIFY(!arrangeTarget(QString(), false));
}

void TestCtlUtil::resolvesPlaylistArguments()
{
    const QJsonArray lists = QJsonDocument::fromJson(
        "[{\"id\": \"aaa-1\", \"name\": \"Calm\"}, {\"id\": \"bbb-2\", \"name\": \"Night\"},"
        " {\"id\": \"ccc-3\", \"name\": \"night\"}, {\"id\": \"ddd-4\", \"name\": \"Day\"}]").array();
    QString error;
    QCOMPARE(resolvePlaylistArg(u"bbb-2"_s, lists, &error), u"bbb-2"_s);   // an id
    QCOMPARE(resolvePlaylistArg(u"Calm"_s, lists, &error), u"aaa-1"_s);    // an exact name
    QCOMPARE(resolvePlaylistArg(u"calm"_s, lists, &error), u"aaa-1"_s);    // ignoring case
    QCOMPARE(resolvePlaylistArg(u"Night"_s, lists, &error), u"bbb-2"_s);   // exact beats ignoring case
    QCOMPARE(resolvePlaylistArg(u"NIGHT"_s, lists, &error), QString());    // ambiguous
    QVERIFY(error.contains(u"more than one"_s));
    QCOMPARE(resolvePlaylistArg(u"nope"_s, lists, &error), QString());
    QVERIFY(error.contains(u"no playlist"_s));
    QCOMPARE(resolvePlaylistArg(u"calm"_s, QJsonArray(), &error), QString());
}

void TestCtlUtil::formatsPlaylistLines()
{
    QCOMPARE(formatPlaylistLine(testutil::json("{\"id\":\"u1\",\"name\":\"Calm\",\"items\":[\"a\",\"b\"],"
                                              "\"order\":\"shuffle\",\"intervalMinutes\":30}")),
             u"u1\tCalm\tshuffle\t30\t2"_s);
}

void TestCtlUtil::buildsPlaylistChanges()
{
    QString error;
    const auto changes = playlistChanges({u"name=My list"_s, u"order=shuffle"_s, u"interval=45"_s,
                                          u"changeOnLogin=on"_s}, &error);
    QVERIFY2(changes, qPrintable(error));
    QCOMPARE(changes->value(u"name"_s).toString(), u"My list"_s);
    QCOMPARE(changes->value(u"order"_s).toString(), u"shuffle"_s);
    QCOMPARE(changes->value(u"intervalMinutes"_s).toInt(), 45);
    QCOMPARE(changes->value(u"changeOnLogin"_s).toBool(), true);
    QCOMPARE(playlistChanges({u"intervalMinutes=5"_s, u"changeOnLogin=false"_s}, &error)->value(u"changeOnLogin"_s).toBool(),
             false);
    QCOMPARE(playlistChanges({u"name=a=b"_s}, &error)->value(u"name"_s).toString(), u"a=b"_s);

    for (const QString &bad : {u"noequals"_s, u"colour=red"_s, u"interval=soon"_s, u"interval=2.5"_s,
                               u"order="_s, u"changeOnLogin=maybe"_s, u"=x"_s}) {
        QVERIFY2(!playlistChanges({bad}, &error), qPrintable(bad));
        QVERIFY(!error.isEmpty());
    }
    QVERIFY(!playlistChanges({}, &error)); // nothing to set
}

void TestCtlUtil::findsPlaylistTargets()
{
    using Target = std::pair<QString, QString>;
    const QJsonArray all = QJsonDocument::fromJson(
        "[{\"output\": \"*\", \"desktop\": \"*\", \"itemId\": \"we:1\"},"
        " {\"output\": \"DP-1\", \"desktop\": \"*\", \"itemId\": \"playlist:a\"},"
        " {\"output\": \"DP-2\", \"desktop\": \"d1\", \"itemId\": \"playlist:b\"},"
        " {\"output\": \"*\", \"desktop\": \"d2\", \"itemId\": \"playlist:c\"}]").array();
    QCOMPARE(playlistTargets(all, QString(), QString()).size(), 3);
    QCOMPARE(playlistTargets(all, u"DP-1"_s, QString()), (QList<Target>{{u"DP-1"_s, u"*"_s}, {u"*"_s, u"d2"_s}}));
    QCOMPARE(playlistTargets(all, u"DP-2"_s, u"d1"_s), (QList<Target>{{u"DP-2"_s, u"d1"_s}}));
    QVERIFY(playlistTargets(all, u"HDMI-1"_s, u"d1"_s).isEmpty());
    QVERIFY(playlistTargets(QJsonArray(), QString(), QString()).isEmpty());
}

void TestCtlUtil::formatsSteamCmdStatus()
{
    QCOMPARE(formatSteamCmdStatus(testutil::json("{\"available\":false,\"state\":\"unavailable\",\"user\":\"\",\"jobs\":[]}")),
             (QStringList{u"unavailable"_s}));
    QCOMPARE(formatSteamCmdStatus(testutil::json("{\"available\":true,\"state\":\"logged-out\",\"user\":\"bob\",\"message\":\"Invalid Password\",\"jobs\":[]}")),
             (QStringList{u"logged-out\tbob\tInvalid Password"_s}));
    QCOMPARE(formatSteamCmdStatus(testutil::json(
                 "{\"available\":true,\"state\":\"logged-in\",\"user\":\"bob\",\"message\":\"\",\"jobs\":["
                 "{\"appId\":\"431960\",\"itemId\":\"1\",\"state\":\"running\",\"progress\":0.456,\"error\":\"\"},"
                 "{\"appId\":\"431960\",\"itemId\":\"2\",\"state\":\"failed\",\"progress\":0,\"error\":\"Access Denied\"}]}")),
             (QStringList{u"logged-in\tbob"_s, u"1\trunning\t46%\t"_s, u"2\tfailed\t0%\tAccess Denied"_s}));
    QCOMPARE(formatSteamCmdStatus(testutil::json("{\"state\":\"logging-in\",\"user\":\"bob\",\"prompt\":\"guard\",\"jobs\":[]}")),
             (QStringList{u"logging-in\tbob\twaiting for guard"_s}));
}

void TestCtlUtil::readsTheAppArgument()
{
    QCOMPARE(steamCmdAppArg(QString()), u"431960"_s); // the default
    QCOMPARE(steamCmdAppArg(u"431960"_s), u"431960"_s);
    QCOMPARE(steamCmdAppArg(u"wallpaper"_s), u"431960"_s);
    QCOMPARE(steamCmdAppArg(u"3474900"_s), u"3474900"_s);
    QCOMPARE(steamCmdAppArg(u"pets"_s), u"3474900"_s);
    QCOMPARE(steamCmdAppArg(u"Anima"_s), u"3474900"_s);
    QCOMPARE(steamCmdAppArg(u"730"_s), QString());
    QCOMPARE(steamCmdAppArg(u"x"_s), QString());
}

void TestCtlUtil::buildsTheImportSpec()
{
    ImportArgs a;
    a.kind = u"sheet"_s;
    a.sources = {u"/tmp/s.png"_s};
    a.frame = u"32x48"_s;
    a.count = u"6"_s;
    a.padding = u"2,3"_s;
    a.spacing = u"1,1"_s;
    a.fps = u"12"_s;
    a.format = u"webp"_s;
    a.keyColor = u"#00ff00"_s;
    a.tolerance = u"30"_s;
    a.keepSource = true;
    QByteArray json;
    QString error;
    QVERIFY2(importSpecJson(a, &json, &error), qPrintable(error));
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    QCOMPARE(o.value(u"kind"_s).toString(), u"sheet"_s);
    QCOMPARE(o.value(u"frameWidth"_s).toInt(), 32);
    QCOMPARE(o.value(u"frameHeight"_s).toInt(), 48);
    QCOMPARE(o.value(u"paddingTop"_s).toInt(), 3);
    QCOMPARE(o.value(u"spacingH"_s).toInt(), 1);
    QCOMPARE(o.value(u"fps"_s).toDouble(), 12.0);
    QCOMPARE(o.value(u"colorKey"_s).toObject().value(u"color"_s).toString(), u"00ff00"_s);
    QCOMPARE(o.value(u"colorKey"_s).toObject().value(u"tolerance"_s).toDouble(), 30.0);
    QVERIFY(o.value(u"keepSource"_s).toBool());
    QCOMPARE(o.value(u"sources"_s).toArray().at(0).toString(), u"/tmp/s.png"_s);
    ImportArgs relative;
    relative.kind = u"gif"_s;
    relative.sources = {u"some/../x.gif"_s};
    QVERIFY(importSpecJson(relative, &json, &error));
    QVERIFY(QJsonDocument::fromJson(json).object().value(u"sources"_s).toArray().at(0).toString().startsWith(u'/'));
    QCOMPARE(formatImportLine(QJsonObject{{u"id"_s, u"imp-1"_s}, {u"state"_s, u"done"_s}, {u"progress"_s, 1.0}, {u"name"_s, u"Neko"_s}, {u"itemId"_s, u"local:1"_s}}),
             u"imp-1\tdone\t100%\tNeko\tlocal:1"_s);
    QCOMPARE(formatImportLine(QJsonObject{{u"id"_s, u"imp-2"_s}, {u"state"_s, u"failed"_s}, {u"progress"_s, 0.25}, {u"name"_s, u"X"_s}, {u"error"_s, u"not an image"_s}}),
             u"imp-2\tfailed\t25%\tX\tnot an image"_s);
}

void TestCtlUtil::buildsTheVideoSpec()
{
    ImportArgs a;
    a.kind = u"video"_s;
    a.sources = {u"/tmp/clip.mp4"_s};
    a.maxSide = u"512"_s;
    a.maxFps = u"15.5"_s;
    a.startSeconds = u"1.5"_s;
    a.durationSeconds = u"2"_s;
    a.keepAudio = true;
    QByteArray json;
    QString error;
    QVERIFY2(importSpecJson(a, &json, &error), qPrintable(error));
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    QCOMPARE(o.value(u"maxSide"_s).toInt(), 512);
    QCOMPARE(o.value(u"maxFps"_s).toDouble(), 15.5);
    QCOMPARE(o.value(u"startMs"_s).toDouble(), 1500.0);
    QCOMPARE(o.value(u"durationMs"_s).toDouble(), 2000.0);
    QVERIFY(o.value(u"keepAudio"_s).toBool());
    for (const auto &[start, duration, fps] : {std::tuple{u"-1"_s, u"1"_s, u"30"_s}, std::tuple{u"x"_s, u"1"_s, u"30"_s}, std::tuple{u"0"_s, u"-2"_s, u"30"_s}, std::tuple{u"0"_s, u"1"_s, u"fast"_s}}) {
        ImportArgs b = a;
        b.startSeconds = start;
        b.durationSeconds = duration;
        b.maxFps = fps;
        QVERIFY(!importSpecJson(b, &json, &error));
        QVERIFY(error.startsWith(u"--"_s));
    }
}

void TestCtlUtil::refusesBadImportOptions()
{
    ImportArgs v;
    v.kind = u"video"_s;
    v.sources = {u"/c.mp4"_s};
    v.durationSeconds = u"0.0004"_s; // would round to 0, which means "to the end"
    QByteArray json;
    QString error;
    QVERIFY(!importSpecJson(v, &json, &error));
    QVERIFY(error.contains(u"--duration"_s));
    v.durationSeconds = u"1e9"_s;
    QVERIFY(!importSpecJson(v, &json, &error));
    ImportArgs g;
    g.kind = u"gif"_s;
    g.sources = {u"/a.gif"_s};
    g.maxFps = u"10"_s;
    QVERIFY(!importSpecJson(g, &json, &error) && error.contains(u"--max-fps"_s));
    g.maxFps.reset();
    g.keepAudio = true;
    QVERIFY(!importSpecJson(g, &json, &error));
    g.keepAudio = false;
    g.fps = u"12"_s;
    QVERIFY(!importSpecJson(g, &json, &error) && error.contains(u"--fps"_s));
    refusesTheRest();
}

void TestCtlUtil::refusesTheRest()
{
    const auto bad = [](auto set, const QString &option) {
        ImportArgs a;
        a.kind = u"sheet"_s;
        a.sources = {u"/x.png"_s};
        set(a);
        QByteArray json;
        QString error;
        QVERIFY2(!importSpecJson(a, &json, &error), qPrintable(option));
        QVERIFY2(error.contains(option), qPrintable(error));
    };
    bad([](ImportArgs &a) { a.frame = u"64"_s; }, u"--frame"_s);
    bad([](ImportArgs &a) { a.frame = u"axb"_s; }, u"--frame"_s);
    bad([](ImportArgs &a) { a.padding = u"1"_s; }, u"--padding"_s);
    bad([](ImportArgs &a) { a.spacing = u"1,x"_s; }, u"--spacing"_s);
    bad([](ImportArgs &a) { a.count = u"4.5"_s; }, u"--count"_s);
    bad([](ImportArgs &a) { a.frameMs = u""_s; }, u"--frame-ms"_s);
    bad([](ImportArgs &a) { a.fps = u"fast"_s; }, u"--fps"_s);
    bad([](ImportArgs &a) { a.size = u"stretch"_s; }, u"--size"_s);
    bad([](ImportArgs &a) { a.format = u"gif"_s; }, u"--format"_s);
    bad([](ImportArgs &a) { a.keyColor = u"green"_s; }, u"--key-color"_s);
    bad([](ImportArgs &a) { a.keyColor = u"00ff0z"_s; }, u"--key-color"_s);
    bad([](ImportArgs &a) { a.keyColor = u"00ff00"_s; a.tolerance = u"lots"_s; }, u"--tolerance"_s);
    bad([](ImportArgs &a) { a.tolerance = u"10"_s; }, u"--key-color"_s);
}

void TestCtlUtil::resolvesNamedArguments()
{
    const QJsonArray list{QJsonObject{{u"id"_s, u"a1"_s}, {u"name"_s, u"Work"_s}}, QJsonObject{{u"id"_s, u"a2"_s}, {u"name"_s, u"play"_s}}, QJsonObject{{u"id"_s, u"a3"_s}, {u"name"_s, u"Play"_s}}};
    QString error;
    QCOMPARE(resolveNamedArg(u"a2"_s, list, u"profile"_s, &error), u"a2"_s);
    QCOMPARE(resolveNamedArg(u"work"_s, list, u"profile"_s, &error), u"a1"_s);
    QCOMPARE(resolveNamedArg(u"play"_s, list, u"profile"_s, &error), u"a2"_s); // the exact name wins over the other case
    QCOMPARE(resolveNamedArg(u"PLAY"_s, list, u"profile"_s, &error), QString());
    QVERIFY(error.contains(u"more than one profile"_s) && error.contains(u"a2"_s) && error.contains(u"a3"_s));
    QCOMPARE(resolveNamedArg(u"nobody"_s, list, u"trigger"_s, &error), QString());
    QVERIFY(error.contains(u"no trigger matches"_s));
}

void TestCtlUtil::formatsAutomationLines()
{
    QCOMPARE(formatProfileLine(QJsonObject{{u"id"_s, u"i"_s}, {u"name"_s, u"Work"_s}, {u"pets"_s, QJsonArray{1, 2}}}, true), u"i\t* Work\t2"_s);
    QCOMPARE(formatProfileLine(QJsonObject{{u"id"_s, u"i"_s}, {u"name"_s, u"Work"_s}}, false), u"i\tWork\t0"_s);
    QCOMPARE(formatTriggerLine(QJsonObject{{u"id"_s, u"t"_s}, {u"enabled"_s, true}, {u"name"_s, u"sleepy"_s}, {u"source"_s, QJsonObject{{u"kind"_s, u"fire"_s}}},
                                           {u"action"_s, QJsonObject{{u"type"_s, u"hide-all"_s}}}}),
             u"t\ton\tsleepy\tfire\thide-all"_s);
    QCOMPARE(formatTimelineLine(QJsonObject{{u"id"_s, u"l"_s}, {u"name"_s, u"wave"_s}, {u"speed"_s, 2}, {u"tracks"_s, QJsonArray{1}}}), u"l\twave\t1\t2"_s);
    QCOMPARE(formatTriggerStateLine(QJsonObject{{u"id"_s, u"t"_s}, {u"enabled"_s, false}, {u"available"_s, false}, {u"outcome"_s, QString()}}), u"t\toff\t- (source unavailable)"_s);
}

void TestCtlUtil::buildsTriggerChanges()
{
    QJsonObject changes;
    QString error;
    QVERIFY2(triggerChangesFromArgs(u"focus"_s, u"app-focus"_s, u"timeline"_s, {u"source.app=firefox*"_s, u"source.on=leave"_s, u"action.id=abc"_s, u"cooldownMs=750"_s, u"enabled=off"_s}, &changes, &error),
             qPrintable(error));
    QCOMPARE(changes.value(u"name"_s).toString(), u"focus"_s);
    QCOMPARE(changes.value(u"source"_s).toObject().value(u"kind"_s).toString(), u"app-focus"_s);
    QCOMPARE(changes.value(u"source"_s).toObject().value(u"app"_s).toString(), u"firefox*"_s);
    QCOMPARE(changes.value(u"action"_s).toObject().value(u"type"_s).toString(), u"timeline"_s);
    QCOMPARE(changes.value(u"cooldownMs"_s).toInt(), 750);
    QCOMPARE(changes.value(u"enabled"_s).toBool(true), false);
    QVERIFY(triggerChangesFromArgs(u"t"_s, u"time"_s, u"hide-all"_s, {u"source.at=22:00"_s, u"source.days=1,2,3"_s}, &changes, &error));
    QCOMPARE(changes.value(u"source"_s).toObject().value(u"days"_s).toArray().size(), 3);
    QVERIFY(triggerChangesFromArgs(u"t"_s, u"interval"_s, u"hide-all"_s, {u"source.everyMinutes=15"_s}, &changes, &error));
    QCOMPARE(changes.value(u"source"_s).toObject().value(u"everyMinutes"_s).toInt(), 15);
    QVERIFY(triggerChangesFromArgs(u"t"_s, u"idle"_s, u"show-all"_s, {u"source.minutes=10"_s, u"source.state=back"_s}, &changes, &error));
    QCOMPARE(changes.value(u"source"_s).toObject().value(u"minutes"_s).toInt(), 10);
    QCOMPARE(changes.value(u"source"_s).toObject().value(u"state"_s).toString(), u"back"_s);
    const auto bad = [&](const QStringList &words, const QString &mention) {
        QVERIFY2(!triggerChangesFromArgs(u"t"_s, u"fire"_s, u"hide-all"_s, words, &changes, &error), qPrintable(words.join(' ')));
        QVERIFY2(error.contains(mention), qPrintable(error));
    };
    bad({u"nonsense"_s}, u"key=value"_s);
    bad({u"app=x"_s}, u"source.KEY="_s);
    bad({u"source.everyMinutes=soon"_s}, u"number"_s);
    bad({u"source.days=mon"_s}, u"days"_s);
    bad({u"enabled=maybe"_s}, u"enabled"_s);
    bad({u"source.=x"_s}, u"names no field"_s);
}

void TestCtlUtil::controlCharactersNeverReachTheTerminal()
{
    const QString hostile = u"evil\x1b[2J\x07name\x7f\x9b"_s;
    QCOMPARE(printable(hostile), u"evil?[2J?name??"_s);
    QCOMPARE(printable(u"ñandú ☃"_s), u"ñandú ☃"_s);
    const QJsonObject named{{u"id"_s, u"i"_s}, {u"name"_s, hostile}, {u"enabled"_s, true}, {u"pets"_s, QJsonArray()}, {u"tracks"_s, QJsonArray()}};
    for (const QString &line : {formatProfileLine(named, true), formatTriggerLine(named), formatTimelineLine(named)}) {
        QVERIFY2(!line.contains(QChar(0x1b)) && !line.contains(QChar(0x07)) && !line.contains(QChar(0x7f)), qPrintable(line));
        QVERIFY(line.contains(u"evil?[2J?name"_s));
    }
    QString error;
    QCOMPARE(resolveNamedArg(hostile, QJsonArray{QJsonObject{{u"id"_s, u"a"_s}, {u"name"_s, u"x"_s}}}, u"profile"_s, &error), QString());
    QVERIFY(!error.contains(QChar(0x1b)));
}

QTEST_GUILESS_MAIN(TestCtlUtil)
#include "tst_ctlutil.moc"
