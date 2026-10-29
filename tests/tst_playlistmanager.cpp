#include "animfixtures.h"
#include "assignment.h"
#include "library.h"
#include "playlist.h"
#include "playlistmanager.h"
#include "testutil.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {
constexpr qint64 kMinute = 60 * 1000;
}

class TestPlaylistManager : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void resolvesTheFirstItemAndReportsThePosition();
    void anAssignmentToAnythingElseIsNotAPlaylist();
    void rotatesWhenTheIntervalElapses();
    void wildcardAssignmentIsOneTargetForEveryOutput();
    void aLongGapRotatesOnceNotInABurst();
    void nothingRotatesWhileLockedAndOnceAfterUnlock();
    void nextAndPreviousMoveAndResetTheTimer();
    void sequentialWrapsAround();
    void shuffleVisitsEveryItemBeforeRepeating();
    void shuffleDoesNotRepeatAcrossBags();
    void missingAndBrokenItemsAreSkipped();
    void aPlaylistWithNoPlayableItemShowsNothing();
    void petsAreSkipped();
    void editingThePlaylistKeepsTheShownItem();
    void removingTheShownItemMovesOn();
    void positionSurvivesARestart();
    void changeOnLoginAdvancesOnceAtStart();
    void anElapsedIntervalAtStartIsNotCaughtUp();
    void corruptStateFileIsKeptAsBackup();
    void unassigningForgetsTheTarget();
    void wakeIsTheEarliestDeadline();
    void aNewIntervalRearmsARunningTarget();
    void aLibraryChangeThatHidesTheShownItemSignals();

private:
    struct Rig {
        QTemporaryDir tmp;
        qint64 now = 1'000'000'000;
        Library library;
        AssignmentStore assignments{tmp.filePath(u"assignments.json"_s)};
        PlaylistStore playlists{tmp.filePath(u"playlists.json"_s)};
        std::unique_ptr<PlaylistManager> manager;
        QString playlistId;

        Rig(int items = 3)
        {
            for (int i = 1; i <= items; ++i)
                testutil::writeVideoItem(tmp.filePath(u"ws"_s), QString::number(i), u"Item %1"_s.arg(i));
            library.setWorkshopDirs({tmp.filePath(u"ws"_s)});
            library.rescan();
            playlists.load();
            assignments.load();
            playlistId = playlists.create(u"Mix"_s);
            Playlist p = *playlists.playlist(playlistId);
            for (int i = 1; i <= items; ++i)
                p.items.append(u"we:%1"_s.arg(i));
            p.intervalMinutes = 10;
            playlists.update(p);
            makeManager();
        }
        void makeManager()
        {
            manager.reset();
            manager = std::make_unique<PlaylistManager>(&playlists, &assignments, &library,
                                                        tmp.filePath(u"state/playlist-state.json"_s));
            manager->setClock([this] { return now; });
            manager->setSeed(7);
            manager->load();
        }
        void assign(const QString &output = u"*"_s, const QString &desktop = u"*"_s)
        {
            assignments.set(*makeAssignment(output, desktop, playlistItemId(playlistId), {}));
        }
        void edit(const std::function<void(Playlist &)> &change)
        {
            Playlist p = *playlists.playlist(playlistId);
            change(p);
            playlists.update(p);
        }
        QString shown(const QString &output = u"*"_s, const QString &desktop = u"*"_s)
        {
            const auto current = manager->current(output, desktop);
            return current ? current->itemId : QString();
        }
        void advance(qint64 ms)
        {
            now += ms;
            manager->checkDue();
        }
    };
};

void TestPlaylistManager::init()
{
}

void TestPlaylistManager::resolvesTheFirstItemAndReportsThePosition()
{
    Rig rig;
    rig.assign();
    const auto current = rig.manager->current(u"DP-1"_s, u"1"_s);
    QVERIFY(current);
    QCOMPARE(current->itemId, u"we:1"_s);
    QCOMPARE(current->playlistId, rig.playlistId);
    QCOMPARE(current->index, 0);
    QCOMPARE(current->nextAt, rig.now + 10 * kMinute);
}

void TestPlaylistManager::anAssignmentToAnythingElseIsNotAPlaylist()
{
    Rig rig;
    rig.assignments.set(*makeAssignment(u"*"_s, u"*"_s, u"we:1"_s, {}));
    QVERIFY(!rig.manager->current(u"DP-1"_s, u"1"_s));
    QVERIFY(!rig.manager->current(u"DP-1"_s, u"1"_s).has_value());
    QString error;
    QVERIFY(!rig.manager->next(u"DP-1"_s, u"1"_s, &error));
    QVERIFY(error.contains(u"playlist"_s));
}

void TestPlaylistManager::rotatesWhenTheIntervalElapses()
{
    Rig rig;
    rig.assign();
    QSignalSpy spy(rig.manager.get(), &PlaylistManager::rotated);
    QCOMPARE(rig.shown(), u"we:1"_s);
    rig.advance(9 * kMinute);
    QCOMPARE(rig.shown(), u"we:1"_s);
    QCOMPARE(spy.count(), 0);
    rig.advance(kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, rig.now + 10 * kMinute);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:3"_s);
    QCOMPARE(spy.count(), 2);
}

void TestPlaylistManager::wildcardAssignmentIsOneTargetForEveryOutput()
{
    Rig rig;
    rig.assign();
    QCOMPARE(rig.shown(u"DP-1"_s, u"1"_s), u"we:1"_s);
    QCOMPARE(rig.shown(u"HDMI-A-1"_s, u"2"_s), u"we:1"_s);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(u"DP-1"_s, u"1"_s), u"we:2"_s);
    QCOMPARE(rig.shown(u"HDMI-A-1"_s, u"2"_s), u"we:2"_s);
}

void TestPlaylistManager::aLongGapRotatesOnceNotInABurst()
{
    Rig rig;
    rig.assign();
    QSignalSpy spy(rig.manager.get(), &PlaylistManager::rotated);
    rig.advance(10 * 60 * kMinute); // asleep for ten hours
    QCOMPARE(rig.shown(), u"we:2"_s);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, rig.now + 10 * kMinute);
    rig.manager->checkDue();
    QCOMPARE(spy.count(), 1);
}

void TestPlaylistManager::nothingRotatesWhileLockedAndOnceAfterUnlock()
{
    Rig rig;
    rig.assign();
    QSignalSpy spy(rig.manager.get(), &PlaylistManager::rotated);
    rig.manager->setSessionLocked(true);
    rig.advance(55 * kMinute);
    QCOMPARE(rig.shown(), u"we:1"_s);
    QCOMPARE(spy.count(), 0);
    rig.manager->setSessionLocked(false); // unlocking runs the rotation that was due
    QCOMPARE(rig.shown(), u"we:2"_s);
    QCOMPARE(spy.count(), 1);
    rig.advance(5 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
}

void TestPlaylistManager::nextAndPreviousMoveAndResetTheTimer()
{
    Rig rig;
    rig.assign();
    rig.advance(4 * kMinute);
    QSignalSpy spy(rig.manager.get(), &PlaylistManager::rotated);
    QString error;
    QVERIFY2(rig.manager->next(u"DP-1"_s, u"1"_s, &error), qPrintable(error));
    QCOMPARE(rig.shown(), u"we:2"_s);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, rig.now + 10 * kMinute);
    QVERIFY(rig.manager->previous(u"DP-1"_s, u"1"_s, &error));
    QCOMPARE(rig.shown(), u"we:1"_s);
    QVERIFY(rig.manager->previous(u"DP-1"_s, u"1"_s, &error));
    QCOMPARE(rig.shown(), u"we:3"_s); // wraps
    QCOMPARE(spy.count(), 3);
}

void TestPlaylistManager::sequentialWrapsAround()
{
    Rig rig;
    rig.assign();
    QStringList seen;
    for (int i = 0; i < 7; ++i) {
        seen.append(rig.shown());
        rig.advance(10 * kMinute);
    }
    QCOMPARE(seen, (QStringList{u"we:1"_s, u"we:2"_s, u"we:3"_s, u"we:1"_s, u"we:2"_s, u"we:3"_s, u"we:1"_s}));
}

void TestPlaylistManager::shuffleVisitsEveryItemBeforeRepeating()
{
    Rig rig(5);
    rig.edit([](Playlist &p) { p.order = PlaylistOrder::Shuffle; });
    rig.assign();
    for (int round = 0; round < 4; ++round) {
        QSet<QString> seen;
        for (int i = 0; i < 5; ++i) {
            seen.insert(rig.shown());
            rig.advance(10 * kMinute);
        }
        QCOMPARE(seen.size(), 5);
    }
}

void TestPlaylistManager::shuffleDoesNotRepeatAcrossBags()
{
    Rig rig(2);
    rig.edit([](Playlist &p) { p.order = PlaylistOrder::Shuffle; });
    rig.assign();
    QString last = rig.shown();
    for (int i = 0; i < 50; ++i) {
        rig.advance(10 * kMinute);
        const QString now = rig.shown();
        QVERIFY2(now != last, qPrintable(u"repeated at step %1"_s.arg(i)));
        last = now;
    }
}

void TestPlaylistManager::missingAndBrokenItemsAreSkipped()
{
    Rig rig;
    rig.edit([](Playlist &p) { p.items = {u"we:ghost"_s, u"we:1"_s, u"we:gone"_s, u"we:2"_s}; });
    rig.assign();
    QCOMPARE(rig.shown(), u"we:1"_s);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:1"_s);
    // a broken project is skipped too
    testutil::writeFile(rig.tmp.filePath(u"ws/2/project.json"_s), "{broken");
    rig.library.rescan();
    QCOMPARE(rig.shown(), u"we:1"_s);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:1"_s);
}

void TestPlaylistManager::aPlaylistWithNoPlayableItemShowsNothing()
{
    Rig rig;
    rig.edit([](Playlist &p) { p.items = {u"we:ghost"_s}; });
    rig.assign();
    const auto current = rig.manager->current(u"*"_s, u"*"_s);
    QVERIFY(current);
    QVERIFY(current->itemId.isEmpty());
    QCOMPARE(current->playlistId, rig.playlistId);
    rig.advance(10 * kMinute); // must not loop forever
    QVERIFY(rig.shown().isEmpty());

    Rig empty;
    empty.edit([](Playlist &p) { p.items.clear(); });
    empty.assign();
    QVERIFY(empty.shown().isEmpty());
    QString error;
    QVERIFY(empty.manager->next(u"*"_s, u"*"_s, &error));
}

void TestPlaylistManager::petsAreSkipped()
{
    Rig rig(1);
    testutil::writeFile(rig.tmp.filePath(u"anima/777/pet.apng"_s), animfixtures::apng(40, 30, {{1, 60}, {1, 60}}));
    rig.library.setAnimaDirs({rig.tmp.filePath(u"anima"_s)});
    rig.library.rescan();
    QVERIFY(rig.library.item(u"anima:777"_s));
    rig.edit([](Playlist &p) { p.items = {u"anima:777"_s, u"we:1"_s}; });
    rig.assign();
    QCOMPARE(rig.shown(), u"we:1"_s);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:1"_s);
}

void TestPlaylistManager::editingThePlaylistKeepsTheShownItem()
{
    Rig rig(4);
    rig.assign();
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    rig.edit([](Playlist &p) { p.items = {u"we:4"_s, u"we:2"_s, u"we:3"_s}; });
    QCOMPARE(rig.shown(), u"we:2"_s);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:3"_s);
    rig.edit([](Playlist &p) { p.order = PlaylistOrder::Shuffle; });
    QCOMPARE(rig.shown(), u"we:3"_s);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->index, 2);
}

void TestPlaylistManager::removingTheShownItemMovesOn()
{
    Rig rig;
    rig.assign();
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    QSignalSpy spy(rig.manager.get(), &PlaylistManager::rotated);
    rig.edit([](Playlist &p) { p.items = {u"we:1"_s, u"we:3"_s}; });
    QCOMPARE(rig.shown(), u"we:3"_s); // the one that followed
    QVERIFY(spy.count() >= 1);
}

void TestPlaylistManager::positionSurvivesARestart()
{
    Rig rig;
    rig.assign();
    rig.advance(10 * kMinute);
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:3"_s);
    const qint64 nextAt = rig.manager->current(u"*"_s, u"*"_s)->nextAt;
    rig.advance(3 * kMinute);
    rig.makeManager();
    QCOMPARE(rig.shown(), u"we:3"_s);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, nextAt);
    rig.advance(7 * kMinute);
    QCOMPARE(rig.shown(), u"we:1"_s);
}

void TestPlaylistManager::changeOnLoginAdvancesOnceAtStart()
{
    Rig rig;
    rig.assign();
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    rig.edit([](Playlist &p) { p.changeOnLogin = true; });
    rig.makeManager();
    QCOMPARE(rig.shown(), u"we:3"_s);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, rig.now + 10 * kMinute);

    // a playlist assigned for the first time starts at its first item, login or not
    Rig fresh;
    fresh.edit([](Playlist &p) { p.changeOnLogin = true; });
    fresh.assign();
    QFile::remove(fresh.tmp.filePath(u"state/playlist-state.json"_s));
    fresh.makeManager();
    QCOMPARE(fresh.shown(), u"we:1"_s);
}

void TestPlaylistManager::anElapsedIntervalAtStartIsNotCaughtUp()
{
    Rig rig;
    rig.assign();
    QCOMPARE(rig.shown(), u"we:1"_s);
    rig.now += 5 * 60 * kMinute; // the daemon was not running
    rig.makeManager();
    QCOMPARE(rig.shown(), u"we:1"_s);
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, rig.now + 10 * kMinute);
}

void TestPlaylistManager::corruptStateFileIsKeptAsBackup()
{
    Rig rig;
    rig.assign();
    rig.advance(10 * kMinute);
    const QString path = rig.tmp.filePath(u"state/playlist-state.json"_s);
    QVERIFY(QFile::exists(path));
    testutil::writeFile(path, "{nope");
    rig.makeManager();
    QCOMPARE(QDir(rig.tmp.filePath(u"state"_s)).entryList({u"playlist-state.json.bad-*"_s}, QDir::Files).size(), 1);
    QCOMPARE(rig.shown(), u"we:1"_s); // starts over
}

void TestPlaylistManager::unassigningForgetsTheTarget()
{
    Rig rig;
    rig.assign();
    rig.advance(10 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    rig.assignments.clear(u"*"_s, u"*"_s);
    QVERIFY(!rig.manager->current(u"*"_s, u"*"_s));
    QCOMPARE(rig.manager->wakeInMs(), -1);
    rig.assign();
    QCOMPARE(rig.shown(), u"we:1"_s);
}

void TestPlaylistManager::wakeIsTheEarliestDeadline()
{
    Rig rig;
    QCOMPARE(rig.manager->wakeInMs(), -1);
    rig.assign(u"*"_s, u"*"_s);
    QCOMPARE(rig.manager->wakeInMs(), int(10 * kMinute));
    rig.now += 4 * kMinute;
    rig.assign(u"DP-1"_s, u"*"_s); // a second target, ten minutes from now
    QCOMPARE(rig.manager->wakeInMs(), int(6 * kMinute));
    rig.advance(6 * kMinute);
    QCOMPARE(rig.manager->wakeInMs(), int(4 * kMinute));
}

void TestPlaylistManager::aNewIntervalRearmsARunningTarget()
{
    Rig rig;
    rig.assign();
    rig.advance(4 * kMinute);
    QCOMPARE(rig.manager->wakeInMs(), int(6 * kMinute));
    rig.edit([](Playlist &p) { p.intervalMinutes = 60; });
    QCOMPARE(rig.manager->wakeInMs(), int(60 * kMinute)); // counted from the edit
    QCOMPARE(rig.manager->current(u"*"_s, u"*"_s)->nextAt, rig.now + 60 * kMinute);
    rig.edit([](Playlist &p) { p.intervalMinutes = 2; });
    QCOMPARE(rig.manager->wakeInMs(), int(2 * kMinute));
    rig.advance(2 * kMinute);
    QCOMPARE(rig.shown(), u"we:2"_s);
    // an edit that leaves the interval alone does not move the deadline
    rig.advance(kMinute);
    rig.edit([](Playlist &p) { p.name = u"Renamed"_s; });
    QCOMPARE(rig.manager->wakeInMs(), int(kMinute));
    // a restart keeps the saved deadline whatever the interval is
    rig.makeManager();
    QCOMPARE(rig.manager->wakeInMs(), int(kMinute));
}

void TestPlaylistManager::aLibraryChangeThatHidesTheShownItemSignals()
{
    Rig rig;
    rig.assign();
    QCOMPARE(rig.shown(), u"we:1"_s);
    QSignalSpy spy(rig.manager.get(), &PlaylistManager::rotated);
    QDir(rig.tmp.filePath(u"ws/1"_s)).removeRecursively();
    rig.library.rescan();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(rig.shown(), u"we:2"_s);
}

QTEST_GUILESS_MAIN(TestPlaylistManager)
#include "tst_playlistmanager.moc"
