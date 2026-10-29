// The overlay's follower of the policy server's PetAudioMuted (src/overlay/surfacemutewatcher.h).

#include "petaudiocontroller.h"
#include "surfacemutewatcher.h"

#include <QTest>
#include <QTimer>

using namespace murasaki;
using namespace Qt::StringLiterals;

namespace {

class FakeSurface : public DBusBackend
{
public:
    using DBusBackend::DBusBackend;
    void start() override { ++starts; }
    void call(const QString &method, const QVariantList &, Callback callback) override
    {
        ++calls;
        Reply reply;
        reply.ok = known && ((method == u"PetAudioMuted"_s && !hasFacts) || (method == u"SessionFacts"_s && hasFacts));
        reply.value = method == u"SessionFacts"_s ? QVariant(facts) : QVariant(muted);
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }
    void property(const QString &, Callback) override {}
    int starts = 0, calls = 0;
    bool muted = false;
    bool known = true;
    bool hasFacts = false; // a surface of revision 16
    QString facts = u"{}"_s;
};

class NullLink final : public PetAudioLink
{
public:
    bool start() override { return up = true; }
    bool running() const override { return up; }
    void send(const QString &line) override { lines << line; }
    void stop() override { up = false; }
    QStringList lines;
    bool up = false;
};

QString id(char c) { return QString(64, QLatin1Char(c)); }

} // namespace

class TstSurfaceMuteWatcher : public QObject
{
    Q_OBJECT

private slots:
    void aMuteFromTheSurfaceSilencesEveryPetAndEndsWithIt()
    {
        NullLink link;
        PetAudioController audio(&link, [] { return qint64(1'000'000'000); });
        PetAudio a;
        a.asset = id('a');
        a.mode = PetAudioMode::Continuous;
        PetAudioController::Pet pet;
        pet.audio = a;
        pet.shown = true;
        pet.output = u"DP-2"_s;
        audio.setPet(u"p"_s, pet);
        link.lines.clear();
        FakeSurface surface;
        SurfaceMuteWatcher watcher(&surface, &audio);
        watcher.start();
        QCOMPARE(surface.starts, 1);
        // the surface appears already muted: read at once
        surface.muted = true;
        emit surface.availabilityChanged(true);
        QTRY_VERIFY(watcher.muted());
        QCOMPARE(link.lines, QStringList{u"gain 0 0"_s});
        // the signal flips it
        link.lines.clear();
        emit surface.signalReceived(u"PetAudioMutedChanged"_s, {false});
        QVERIFY(!watcher.muted());
        QCOMPARE(link.lines, QStringList{u"gain 0 750"_s});
        emit surface.signalReceived(u"PetAudioMutedChanged"_s, {true});
        QVERIFY(watcher.muted());
        // the policy server goes away: the rule is off
        link.lines.clear();
        emit surface.availabilityChanged(false);
        QVERIFY(!watcher.muted());
        QCOMPARE(link.lines, QStringList{u"gain 0 750"_s});
    }
    void aSurfaceWithFactsMutesOnlyTheOutputsThatHaveAFullscreenWindow()
    {
        NullLink link;
        PetAudioController audio(&link, [] { return qint64(1'000'000'000); });
        FakeSurface surface;
        surface.hasFacts = true;
        surface.facts = u"{\"quiet\":false,\"outputs\":{\"DP-1\":{\"fullscreen\":false},\"DP-2\":{\"fullscreen\":true,\"app\":\"mpv\"}}}"_s;
        SurfaceMuteWatcher watcher(&surface, &audio);
        emit surface.availabilityChanged(true);
        QTRY_VERIFY(watcher.muted());
        QCOMPARE(watcher.mutedOutputs(), QSet<QString>{u"DP-2"_s});
        // the facts change: the fullscreen window moves to the other output
        surface.facts = u"{\"quiet\":false,\"outputs\":{\"DP-1\":{\"fullscreen\":true},\"DP-2\":{\"fullscreen\":false}}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_COMPARE(watcher.mutedOutputs(), QSet<QString>{u"DP-1"_s});
        // a game or a listed application silences every output
        surface.facts = u"{\"quiet\":true,\"outputs\":{}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_COMPARE(watcher.mutedOutputs(), QSet<QString>{u"*"_s});
        // the old single answer is ignored once the facts exist
        emit surface.signalReceived(u"PetAudioMutedChanged"_s, {false});
        QCOMPARE(watcher.mutedOutputs(), QSet<QString>{u"*"_s});
        // nothing fullscreen, nothing quiet
        surface.facts = u"{\"quiet\":false,\"outputs\":{\"DP-1\":{\"fullscreen\":false}}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_VERIFY(!watcher.muted());
        // a garbled answer mutes nothing
        surface.facts = u"not json"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTest::qWait(30);
        QVERIFY(!watcher.muted());
    }
    void theGameScreensAreTheCoveredOutputsWhileAGamePlays()
    {
        NullLink link;
        PetAudioController audio(&link);
        FakeSurface surface;
        surface.hasFacts = true;
        // a game on DP-2 (fullscreen); HDMI-A-1 shows the desktop
        surface.facts = u"{\"quiet\":true,\"known\":true,\"gameRunning\":true,\"outputs\":{\"DP-2\":{\"fullscreen\":true},\"HDMI-A-1\":{\"fullscreen\":false,\"occluded\":false}}}"_s;
        SurfaceMuteWatcher watcher(&surface, &audio);
        QSet<QString> screens;
        int told = 0;
        connect(&watcher, &SurfaceMuteWatcher::gameScreensChanged, this, [&](const QSet<QString> &outputs) {
            screens = outputs;
            ++told;
        });
        emit surface.availabilityChanged(true);
        QTRY_COMPARE(screens, QSet<QString>{u"DP-2"_s});
        // a window that covers a screen counts as the wallpaper's rule counts it
        surface.facts = u"{\"quiet\":true,\"known\":true,\"gameRunning\":true,\"outputs\":{\"DP-2\":{\"fullscreen\":true},\"HDMI-A-1\":{\"occluded\":true}}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_COMPARE(screens, (QSet<QString>{u"DP-2"_s, u"HDMI-A-1"_s}));
        // a session that cannot say which windows cover what: every screen
        surface.facts = u"{\"quiet\":true,\"known\":false,\"gameRunning\":true,\"outputs\":{}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_COMPARE(screens, QSet<QString>{u"*"_s});
        // the game ends: none, and the same answer again is not told twice
        surface.facts = u"{\"quiet\":false,\"known\":true,\"gameRunning\":false,\"outputs\":{\"DP-2\":{\"fullscreen\":true}}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_VERIFY(screens.isEmpty());
        const int before = told;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTest::qWait(30);
        QCOMPARE(told, before);
        // the surface goes away while a game plays: none
        surface.facts = u"{\"quiet\":true,\"known\":true,\"gameRunning\":true,\"outputs\":{\"DP-2\":{\"fullscreen\":true}}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_COMPARE(screens, QSet<QString>{u"DP-2"_s});
        emit surface.availabilityChanged(false);
        QVERIFY(screens.isEmpty());
    }
    void theFullscreenModeOfThePetsIsToldWhichOutputsHaveAFullscreenWindowAndWhich()
    {
        NullLink link;
        PetAudioController audio(&link);
        FakeSurface surface;
        surface.hasFacts = true;
        surface.facts = u"{\"quiet\":false,\"known\":true,\"outputs\":{\"DP-1\":{\"fullscreen\":false},\"DP-2\":{\"fullscreen\":true,\"app\":\"mpv\"}}}"_s;
        SurfaceMuteWatcher watcher(&surface, &audio);
        QHash<QString, QString> apps;
        bool known = false;
        int told = 0;
        connect(&watcher, &SurfaceMuteWatcher::fullscreenChanged, this, [&](const QHash<QString, QString> &a, bool k) {
            apps = a;
            known = k;
            ++told;
        });
        emit surface.availabilityChanged(true);
        QTRY_VERIFY(told > 0);
        QCOMPARE(apps, (QHash<QString, QString>{{u"DP-2"_s, u"mpv"_s}}));
        QVERIFY(known);
        // a session that cannot say: known false, and nothing is claimed
        surface.facts = u"{\"quiet\":false,\"known\":false,\"outputs\":{}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_VERIFY(!known);
        QVERIFY(apps.isEmpty());
        // a facts object that says nothing of `known` (a caller that is not trusted, or an old surface) is unknown
        surface.facts = u"{\"quiet\":false,\"outputs\":{}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTest::qWait(30);
        QVERIFY(!known);
        // a game or a listed application: every output, no application
        surface.facts = u"{\"quiet\":true,\"known\":true,\"outputs\":{}}"_s;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_VERIFY(apps.contains(u"*"_s));
        QVERIFY(known);
        // the surface goes away: nothing is fullscreen and nothing is known
        emit surface.availabilityChanged(false);
        QVERIFY(apps.isEmpty());
        QVERIFY(!known);
        // an older surface (the single answer): "*" with no application, known because it answered
        surface.hasFacts = false;
        surface.muted = true;
        emit surface.availabilityChanged(true);
        QTRY_VERIFY(apps.contains(u"*"_s));
        QVERIFY(known);
    }
    void aSurfaceThatStopsAnsweringSessionFactsIsReadTheOldWayAgain()
    {
        NullLink link;
        PetAudioController audio(&link);
        FakeSurface surface;
        surface.hasFacts = true;
        surface.facts = u"{\"quiet\":false,\"outputs\":{\"DP-2\":{\"fullscreen\":true}}}"_s;
        SurfaceMuteWatcher watcher(&surface, &audio);
        emit surface.availabilityChanged(true);
        QTRY_VERIFY(watcher.muted());
        // the surface is replaced by an older one (no SessionFacts): its single answer rules, and so do its signals
        surface.hasFacts = false;
        surface.muted = false;
        emit surface.signalReceived(u"SessionFactsChanged"_s, {});
        QTRY_VERIFY(!watcher.muted());
        emit surface.signalReceived(u"PetAudioMutedChanged"_s, {true});
        QVERIFY(watcher.muted());
        QCOMPARE(watcher.mutedOutputs(), QSet<QString>{u"*"_s});
    }
    void anOlderSurfaceLeavesTheRuleOffAndAStaleReadDoesNotOverrideASignal()
    {
        NullLink link;
        PetAudioController audio(&link);
        FakeSurface old;
        old.known = false;
        SurfaceMuteWatcher w(&old, &audio);
        emit old.availabilityChanged(true);
        QTest::qWait(30);
        QVERIFY(!w.muted());
        FakeSurface surface;
        surface.muted = true;
        SurfaceMuteWatcher v(&surface, &audio);
        emit surface.availabilityChanged(true);               // a read is in flight (answers true)
        emit surface.signalReceived(u"PetAudioMutedChanged"_s, {false}); // and a newer fact arrives first
        QTest::qWait(30);
        QVERIFY(!v.muted());
    }
};

QTEST_GUILESS_MAIN(TstSurfaceMuteWatcher)
#include "tst_surfacemutewatcher.moc"
