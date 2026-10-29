#include "animfixtures.h"
#include "constants.h"
#include "localpets.h"
#include "localpuppets.h"
#include "murasakiservice.h"
#include "petaudiomanager.h"
#include "testutil.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// `murasakictl pets ...` against a daemon facade that owns the real service name. That is only safe on a private
// bus, so the test runs under dbus-run-session with MURASAKI_TEST_PRIVATE_BUS=1 and skips otherwise.
class TestCtlPets : public QObject
{
    Q_OBJECT

private slots:
    void petsCommands();
    void usageErrors();
    void soundCommands();
    void puppetCommands();

private:
    struct Result {
        int code = -1;
        QString out;
        QString err;
    };
    // The service lives in this process, so the event loop must keep running while ctl waits for its reply.
    static Result ctl(const QStringList &args)
    {
        QProcess process;
        process.start(QStringLiteral(MURASAKICTL_PATH), args);
        if (!process.waitForStarted(5000))
            return {};
        if (!QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, 15000)) {
            process.kill();
            process.waitForFinished(2000);
            return {-2, {}, u"timed out"_s};
        }
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()),
                QString::fromUtf8(process.readAllStandardError())};
    }
};

void TestCtlPets::petsCommands()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        QSKIP("no D-Bus session bus");

    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"anima/9/idle.apng"_s), animfixtures::apng(40, 30, {{1, 10}, {1, 10}}));
    Library library;
    library.setAnimaDirs({tmp.filePath(u"anima"_s)});
    library.rescan();
    AssignmentStore assignments(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    PetStore pets(tmp.filePath(u"pets.json"_s));
    TranscodeManager transcodes(tmp.filePath(u"cache/pets"_s));
    MurasakiService service(&library, &assignments, &settings, u"ok"_s, {});
    service.attachPets(&pets, &transcodes);
    const auto cleanup = qScopeGuard([&] {
        bus.unregisterService(QString(kDBusService));
        bus.unregisterObject(QString(kDBusPath));
    });
    QVERIFY(service.registerObject(bus));
    QVERIFY(bus.registerService(QString(kDBusService)));

    QCOMPARE(ctl({u"pets"_s, u"list"_s}).out, QString());

    const Result added = ctl({u"pets"_s, u"add"_s, u"anima:9"_s, u"--output"_s, u"DP-2"_s, u"--x"_s, u"15"_s,
                              u"--y"_s, u"25.5"_s, u"--width"_s, u"200"_s, u"--fps"_s, u"24"_s});
    QCOMPARE(added.code, 0);
    const QString petId = added.out.trimmed();
    QVERIFY(!petId.isEmpty());
    QCOMPARE(pets.pets().size(), 1);
    QCOMPARE(pets.pets().at(0).output, u"DP-2"_s);
    QCOMPARE(pets.pets().at(0).x, 15.0);
    QCOMPARE(pets.pets().at(0).y, 25.5);
    QCOMPARE(pets.pets().at(0).width, 200.0);
    QCOMPARE(pets.pets().at(0).variant, u"idle"_s);
    QCOMPARE(pets.pets().at(0).fps, 24);

    QCOMPARE(ctl({u"pets"_s, u"list"_s}).out, petId + u"\tanima:9\tidle\tDP-2\t15\t25.5\t200\t1\t24\tplaying\t1\n"_s);
    // Left out, a pet steps at the default rate, Anima Engine's too.
    const Result plain = ctl({u"pets"_s, u"add"_s, u"anima:9"_s});
    QCOMPARE(plain.code, 0);
    QCOMPARE(pets.pets().at(1).fps, 24);
    QCOMPARE(pets.pets().at(1).speed, 1.0);
    // The timing of a pet, in its three modes.
    const QString second = plain.out.trimmed();
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"file"_s}).code, 0);
    QCOMPARE(pets.pets().at(1).fps, 0);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"fixed"_s}).code, 0);
    QCOMPARE(pets.pets().at(1).fps, 24);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"fixed"_s, u"15"_s}).code, 0);
    QCOMPARE(pets.pets().at(1).fps, 15);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"speed"_s, u"2"_s}).code, 0);
    QCOMPARE(pets.pets().at(1).fps, 0);
    QCOMPARE(pets.pets().at(1).speed, 2.0);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"fixed"_s, u"120"_s}).code, 0);
    QCOMPARE(pets.pets().at(1).fps, 120);
    QCOMPARE(pets.pets().at(1).speed, 1.0);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"file"_s}).code, 0);
    QCOMPARE(pets.pets().at(1).timing(), (PetTiming{0, 1}));
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"speed"_s, u"9"_s}).code, 1);   // the daemon refuses a speed out of range
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"fixed"_s, u"121"_s}).code, 1);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, second, u"warp"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"timing"_s, u"nope"_s, u"file"_s}).code, 1);
    QCOMPARE(pets.pets().at(1).timing(), (PetTiming{0, 1}));
    const Result sped = ctl({u"pets"_s, u"add"_s, u"anima:9"_s, u"--speed"_s, u"0.5"_s});
    QCOMPARE(sped.code, 0);
    QCOMPARE(pets.pets().at(2).speed, 0.5);
    QCOMPARE(ctl({u"pets"_s, u"add"_s, u"anima:9"_s, u"--speed"_s, u"slow"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"add"_s, u"anima:9"_s, u"--fps"_s, u"15"_s, u"--speed"_s, u"2"_s}).code, 1); // not together
    QCOMPARE(ctl({u"pets"_s, u"remove"_s, sped.out.trimmed()}).code, 0);
    QCOMPARE(ctl({u"pets"_s, u"add"_s, u"anima:9"_s, u"--fps"_s, u"121"_s}).code, 1);
    QCOMPARE(ctl({u"pets"_s, u"add"_s, u"anima:9"_s, u"--fps"_s, u"fast"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"remove"_s, plain.out.trimmed()}).code, 0);

    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"on"_s}).out, u"on\n"_s);
    QVERIFY(service.petsArrange());
    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"toggle"_s}).out, u"off\n"_s);
    QVERIFY(!service.petsArrange());
    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"toggle"_s}).out, u"on\n"_s);
    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"off"_s}).out, u"off\n"_s);

    // the interaction with the wallpaper, and the arrange mode that wins over it
    QVERIFY(!service.wallpaperInteract());
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s, u"on"_s}).out, u"on\n"_s);
    QVERIFY(service.wallpaperInteract());
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s, u"toggle"_s}).out, u"off\n"_s);
    QVERIFY(!service.wallpaperInteract());
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s, u"toggle"_s}).out, u"on\n"_s);
    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"on"_s}).code, 0);
    QVERIFY(!service.wallpaperInteract()); // arranging turned it off
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s, u"on"_s}).code, 1); // and keeps it off
    QVERIFY(!service.wallpaperInteract());
    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"off"_s}).code, 0);
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s, u"maybe"_s}).code, 2);
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s}).code, 2);
    QCOMPARE(ctl({u"wallpaper"_s, u"interact"_s, u"off"_s}).out, u"off\n"_s);

    // hide, show, toggle and rescue: nothing is printed on success
    QVERIFY(!service.petsHidden());
    const Result hid = ctl({u"pets"_s, u"hide"_s});
    QCOMPARE(hid.code, 0);
    QCOMPARE(hid.out, QString());
    QVERIFY(service.petsHidden());
    QCOMPARE(ctl({u"pets"_s, u"toggle"_s}).code, 0);
    QVERIFY(!service.petsHidden());
    QCOMPARE(ctl({u"pets"_s, u"toggle-hidden"_s}).code, 0);
    QVERIFY(service.petsHidden());
    QCOMPARE(ctl({u"pets"_s, u"show"_s}).out, QString());
    QVERIFY(!service.petsHidden());
    QCOMPARE(ctl({u"pets"_s, u"hide"_s}).code, 0);
    QCOMPARE(ctl({u"pets"_s, u"hide"_s}).code, 0); // already hidden: fine
    QVERIFY(service.petsHidden());
    QCOMPARE(ctl({u"pets"_s, u"update-nothing"_s}).code, 2);
    // the rescue: click-through off and the pets shown again
    QVERIFY(pets.pets().at(0).clickThrough);
    const Result rescued = ctl({u"pets"_s, u"rescue"_s});
    QCOMPARE(rescued.code, 0);
    QCOMPARE(rescued.out, QString());
    QVERIFY(!pets.pets().at(0).clickThrough);
    QVERIFY(!service.petsHidden());
    QCOMPARE(ctl({u"pets"_s, u"hide"_s, u"extra"_s}).code, 2);

    // pets set: the behaviour fields, validated by the daemon
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--click-through"_s, u"on"_s}).code, 0); // (the rescue above turned it off)
    QVERIFY(pets.pets().at(0).clickThrough);
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--click-through"_s, u"off"_s, u"--lock"_s, u"on"_s, u"--rotation"_s, u"450"_s, u"--flip-h"_s, u"on"_s}).code, 0);
    QVERIFY(!pets.pets().at(0).clickThrough && pets.pets().at(0).locked && pets.pets().at(0).flipH);
    QCOMPARE(pets.pets().at(0).rotation, 90.0); // wrapped by the daemon
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--hover-hide"_s, u"on"_s}).code, 0);
    QVERIFY(pets.pets().at(0).hoverHide);
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--focus-fade"_s, u"on"_s}).code, 0);
    QVERIFY(pets.pets().at(0).focusFade && !pets.pets().at(0).hoverHide); // the other one went off
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--smart-dodge"_s, u"on"_s, u"--keep-above"_s, u"off"_s}).code, 0);
    QVERIFY(pets.pets().at(0).smartDodge && !pets.pets().at(0).keepAbove);
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--hover-hide"_s, u"on"_s, u"--focus-fade"_s, u"on"_s}).code, 1); // a contradiction: the daemon refuses
    QCOMPARE(ctl({u"pets"_s, u"set"_s, u"nope"_s, u"--lock"_s, u"off"_s}).code, 1);                               // unknown pet
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--lock"_s, u"maybe"_s}).code, 2);                                 // not on/off: usage
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--rotation"_s, u"far"_s}).code, 2);
    // the real parser: a negative angle is the value of --rotation, in both spellings, and the daemon wraps it
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--rotation"_s, u"-10"_s}).code, 0);
    QCOMPARE(pets.pets().at(0).rotation, 350.0);
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--rotation=-100"_s}).code, 0);
    QCOMPARE(pets.pets().at(0).rotation, 260.0);
    // an empty value is given and wrong: exit 2 even when another option would do (and nothing is changed)
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--lock"_s, u""_s, u"--rotation"_s, u"10"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--rotation"_s, u""_s, u"--lock"_s, u"off"_s}).code, 2);
    QCOMPARE(pets.pets().at(0).rotation, 260.0);
    QVERIFY(pets.pets().at(0).locked);
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId}).code, 2);                                                         // nothing to set
    QCOMPARE(ctl({u"pets"_s, u"set"_s}).code, 2);
    QVERIFY(pets.pets().at(0).locked); // the failed ones changed nothing

    // pets state and pets track
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"grabbed"_s, u"variant"_s, u"small"_s}).code, 0);
    QCOMPARE(pets.pets().at(0).states.of(PetState::Grabbed)->variant, u"small"_s);
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"idle"_s, u"sequence"_s, u"big,small"_s, u"--order"_s, u"random"_s, u"--advance"_s, u"click"_s, u"--once"_s, u"--hold-ms"_s, u"1000"_s}).code, 0);
    const PetSequence sequence = pets.pets().at(0).states.of(PetState::Idle)->sequence;
    QCOMPARE(sequence.items, (QStringList{u"big"_s, u"small"_s}));
    QVERIFY(sequence.order == PetSequenceOrder::Random && sequence.advance == PetSequenceAdvance::Click && sequence.once);
    QCOMPARE(sequence.holdMs, 1000);
    QVERIFY(ctl({u"pets"_s, u"list"_s}).out.contains(u"\tstates:idle,grabbed"_s));
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"clear"_s, u"idle"_s}).code, 0);
    QVERIFY(!pets.pets().at(0).states.of(PetState::Idle) && pets.pets().at(0).states.of(PetState::Grabbed));
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"clear"_s}).code, 0);
    QVERIFY(pets.pets().at(0).states.isDefault());
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"sleeping"_s, u"variant"_s, u"small"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"idle"_s, u"sequence"_s, u"a,,b"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId, u"idle"_s, u"variant"_s, u"small"_s, u"--once"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"state"_s, petId}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"state"_s, u"nope"_s, u"idle"_s, u"variant"_s, u"small"_s}).code, 1); // unknown pet
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"follow"_s}).code, 1); // the pet dodges (set above): the daemon refuses and says so
    QCOMPARE(ctl({u"pets"_s, u"set"_s, petId, u"--smart-dodge"_s, u"off"_s, u"--focus-fade"_s, u"off"_s}).code, 0);
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"follow"_s, u"--distance"_s, u"300"_s, u"--speed"_s, u"2"_s, u"--no-return"_s}).code, 0);
    QVERIFY(pets.pets().at(0).tracking.mode == PetTrackingMode::Follow && pets.pets().at(0).tracking.distance == 300 && pets.pets().at(0).tracking.speed == 2 && !pets.pets().at(0).tracking.returnHome);
    QVERIFY(ctl({u"pets"_s, u"list"_s}).out.contains(u"\ttracking:follow"_s));
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"look"_s}).code, 0);
    QCOMPARE(pets.pets().at(0).tracking.distance, 300.0); // key by key
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"off"_s}).code, 0);
    QVERIFY(!pets.pets().at(0).tracking.active());
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"chase"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"follow"_s, u"--distance"_s, u"far"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"follow"_s, u"--distance"_s, u"9000"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId, u"follow"_s, u"--return"_s, u"--no-return"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"track"_s, petId}).code, 2);

    const Result unknown = ctl({u"pets"_s, u"add"_s, u"anima:404"_s});
    QCOMPARE(unknown.code, 1);
    QVERIFY(unknown.err.contains(u"Unknown item"_s));
    QCOMPARE(ctl({u"pets"_s, u"remove"_s, u"nope"_s}).code, 1);
    QCOMPARE(ctl({u"pets"_s, u"remove"_s, petId}).code, 0);
    QVERIFY(pets.pets().isEmpty());
}

static QByteArray tinyWav()
{
    QByteArray data;
    for (int i = 0; i < 2205; ++i) {
        const qint16 v = qint16(3000 * ((i / 25) % 2 ? 1 : -1));
        data.append(char(v & 0xff));
        data.append(char((v >> 8) & 0xff));
    }
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    return "RIFF" + le32(36 + quint32(data.size())) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(22050) + le32(44100) + le16(2) + le16(16) + "data"
        + le32(quint32(data.size())) + data;
}

void TestCtlPets::soundCommands()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        QSKIP("no D-Bus session bus");
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"anima/9/idle.apng"_s), animfixtures::apng(40, 30, {{1, 10}, {1, 10}}));
    testutil::writeFile(tmp.filePath(u"tone.wav"_s), tinyWav());
    Library library;
    library.setAnimaDirs({tmp.filePath(u"anima"_s)});
    library.rescan();
    AssignmentStore assignments(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    PetStore pets(tmp.filePath(u"pets.json"_s));
    TranscodeManager transcodes(tmp.filePath(u"cache/pets"_s));
    PetAudioManager audio(tmp.filePath(u"data/pets/audio"_s), QStringLiteral(MURASAKI_PETAUDIO_BIN), QString());
    audio.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
    MurasakiService service(&library, &assignments, &settings, u"ok"_s, {});
    service.attachPets(&pets, &transcodes);
    service.attachPetAudio(&audio);
    const auto cleanup = qScopeGuard([&] {
        bus.unregisterService(QString(kDBusService));
        bus.unregisterObject(QString(kDBusPath));
    });
    QVERIFY(service.registerObject(bus));
    QVERIFY(bus.registerService(QString(kDBusService)));

    const QString petId = ctl({u"pets"_s, u"add"_s, u"anima:9"_s}).out.trimmed();
    QVERIFY(!petId.isEmpty());
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"list"_s}).out, QString());
    const Result imported = ctl({u"pets"_s, u"audio"_s, u"import"_s, tmp.filePath(u"tone.wav"_s)});
    QCOMPARE(imported.code, 0);
    const QString asset = imported.out.trimmed();
    QCOMPARE(asset.size(), 64);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"list"_s}).out, asset + u"\ttone.wav\t100\t0\n"_s);
    // a file that is not a sound is refused with the name
    testutil::writeFile(tmp.filePath(u"noise.mp3"_s), QByteArray(3000, 'x'));
    const Result bad = ctl({u"pets"_s, u"audio"_s, u"import"_s, tmp.filePath(u"noise.mp3"_s)});
    QCOMPARE(bad.code, 1);
    QVERIFY2(bad.err.contains(u"audio: not audio"_s), qPrintable(bad.err));
    // give it to a pet
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId, u"--asset"_s, asset, u"--mode"_s, u"click"_s, u"--volume"_s, u"30"_s, u"--offset"_s, u"-200"_s}).code, 0);
    QCOMPARE(pets.pets().at(0).audio.asset, asset);
    QCOMPARE(pets.pets().at(0).audio.mode, PetAudioMode::Click);
    QCOMPARE(pets.pets().at(0).audio.volume, 30);
    QCOMPARE(pets.pets().at(0).audio.offsetMs, -200);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId, u"--mode"_s, u"once"_s}).code, 0); // only the mode
    QCOMPARE(pets.pets().at(0).audio.volume, 30);
    // --file imports and assigns in one go (the same file is the same sound)
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId, u"--file"_s, tmp.filePath(u"tone.wav"_s)}).code, 0);
    QCOMPARE(pets.pets().at(0).audio.asset, asset);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"list"_s}).out, asset + u"\ttone.wav\t100\t1\n"_s);
    // usage errors
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId, u"--mode"_s, u"loud"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId, u"--asset"_s, asset, u"--file"_s, u"x"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"set"_s, petId, u"--volume"_s, u"101"_s}).code, 1); // the daemon checks the range
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"nothing"_s}).code, 2);
    // the asset is in use: it stays
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"remove"_s, asset}).code, 1);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"gc"_s}).out, u"0\n"_s);
    // the master
    QCOMPARE(ctl({u"pets"_s, u"volume"_s, u"40"_s}).code, 0);
    QCOMPARE(settings.values().value(u"petsVolume"_s).toInt(), 40);
    QCOMPARE(ctl({u"pets"_s, u"mute"_s}).code, 0);
    QVERIFY(settings.values().value(u"petsMuted"_s).toBool());
    QCOMPARE(ctl({u"pets"_s, u"unmute"_s}).code, 0);
    QVERIFY(!settings.values().value(u"petsMuted"_s).toBool());
    QCOMPARE(ctl({u"pets"_s, u"volume"_s, u"101"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"volume"_s, u"loud"_s}).code, 2);
    // the overlay is not running here: testing says so
    const Result noOverlay = ctl({u"pets"_s, u"audio"_s, u"test"_s, petId});
    QCOMPARE(noOverlay.code, 1);
    QVERIFY(noOverlay.err.contains(u"murasaki-overlay"_s));
    // taking the sound off frees the asset
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"off"_s, petId}).code, 0);
    QVERIFY(pets.pets().at(0).audio.isDefault());
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"remove"_s, asset}).code, 0);
    QCOMPARE(ctl({u"pets"_s, u"audio"_s, u"list"_s}).out, QString());
}

void TestCtlPets::puppetCommands()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        QSKIP("no D-Bus session bus");

    QTemporaryDir tmp;
    const QString folder = tmp.filePath(u"puppets/haru"_s);
    testutil::writeFile(folder + u"/model.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    Library library;
    library.rescan();
    AssignmentStore assignments(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    PetStore pets(tmp.filePath(u"pets.json"_s));
    TranscodeManager transcodes(tmp.filePath(u"cache/pets"_s));
    LocalPetStore localPuppets(tmp.filePath(u"local-puppets.json"_s));
    localPuppets.setItemIdFunction(&LocalPuppetImporter::itemIdFor);
    MurasakiService service(&library, &assignments, &settings, u"ok"_s, {});
    service.attachPets(&pets, &transcodes);
    service.attachLocalPuppets(&localPuppets, nullptr);
    const auto cleanup = qScopeGuard([&] {
        bus.unregisterService(QString(kDBusService));
        bus.unregisterObject(QString(kDBusPath));
    });
    QVERIFY(service.registerObject(bus));
    QVERIFY(bus.registerService(QString(kDBusService)));

    QCOMPARE(ctl({u"pets"_s, u"puppet"_s, u"list"_s}).out, QString());
    const Result added = ctl({u"pets"_s, u"puppet"_s, u"add"_s, folder});
    QCOMPARE(added.code, 0);
    const QString id = added.out.trimmed();
    QCOMPARE(id, LocalPuppetImporter::itemIdFor(testutil::canonical(folder)));
    const Result listed = ctl({u"pets"_s, u"puppet"_s, u"list"_s});
    QCOMPARE(listed.code, 0);
    QVERIFY2(listed.out.startsWith(id + u"\tok\topen\tmodel\tunchecked"_s), qPrintable(listed.out));
    // a pet can show it, and the item cannot be removed meanwhile
    const Result pet = ctl({u"pets"_s, u"add"_s, id, u"--variant"_s, u"model"_s});
    QCOMPARE(pet.code, 0);
    QVERIFY(ctl({u"pets"_s, u"puppet"_s, u"remove"_s, id}).code != 0);
    QCOMPARE(ctl({u"pets"_s, u"remove"_s, pet.out.trimmed()}).code, 0);
    QCOMPARE(ctl({u"pets"_s, u"puppet"_s, u"remove"_s, id}).code, 0);
    QCOMPARE(ctl({u"pets"_s, u"puppet"_s, u"list"_s}).out, QString());
    // what is not a puppet, and a usage that is not one
    QVERIFY(ctl({u"pets"_s, u"puppet"_s, u"add"_s, tmp.filePath(u"puppets"_s)}).code != 0);
    QCOMPARE(ctl({u"pets"_s, u"puppet"_s, u"add"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"puppet"_s, u"frobnicate"_s}).code, 2);
    // the check on a folder prints a line (the helper is the build's; the verdict is for a folder that is not read as a puppet here)
    qputenv("MURASAKI_PUPPET", QByteArray(MURASAKI_PUPPET_PATH));
    const Result checked = ctl({u"pets"_s, u"puppet"_s, u"check"_s, tmp.filePath(u"puppets"_s)});
    QVERIFY2(checked.out.startsWith(u"{\"ok\":false"_s), qPrintable(checked.out + checked.err));
    QVERIFY(checked.code != 0);
    // with the sandbox missing and importRequireSandbox on (the default) the parser is never run bare
    qputenv("MURASAKI_SANDBOX", "/nonexistent/murasaki-sandbox");
    const Result bare = ctl({u"pets"_s, u"puppet"_s, u"check"_s, tmp.filePath(u"puppets"_s)});
    qunsetenv("MURASAKI_SANDBOX");
    QVERIFY(bare.code != 0);
    QVERIFY2(bare.err.contains(u"sandbox unavailable"_s), qPrintable(bare.err));
    QVERIFY2(bare.out.isEmpty(), qPrintable(bare.out));
    qunsetenv("MURASAKI_PUPPET");
}

void TestCtlPets::usageErrors()
{
    // These never reach a daemon, so they are safe anywhere.
    QCOMPARE(ctl({u"pets"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"arrange"_s, u"maybe"_s}).code, 2);
    QCOMPARE(ctl({u"pets"_s, u"add"_s}).code, 2);
    const Result badNumber = ctl({u"pets"_s, u"add"_s, u"anima:1"_s, u"--x"_s, u"left"_s});
    QCOMPARE(badNumber.code, 2);
    QVERIFY(badNumber.err.contains(u"--x"_s));
}

QTEST_GUILESS_MAIN(TestCtlPets)
#include "tst_ctlpets.moc"
