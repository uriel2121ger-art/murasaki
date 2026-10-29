#include "daemonclient.h"
#include "fakebackend.h"
#include "librarymodel.h"
#include "usenowflow.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::DaemonClient;
using murasaki::LibraryModel;
using murasaki::UseNowFlow;

// The Workshop's "Use now": apply an item to all screens, only say so once the daemon agrees, wait for an item that
// Steam has just finished, and clear (or leave) the settings of single screens that would hide it.
class TstUseNowFlow : public QObject
{
    Q_OBJECT

private:
    struct Rig {
        FakeBackend *backend = new FakeBackend;
        DaemonClient client{backend};
        LibraryModel library{&client};
        UseNowFlow flow{&client, &library};

        explicit Rig(const QByteArray &items = R"([{"id":"we:1","title":"One"}])",
                     const QByteArray &assignments = "[]")
        {
            backend->setProperty(u"ApiVersion"_s, 7u);
            backend->setProperty(u"Status"_s, u"ok"_s);
            backend->setProperty(u"WallpaperEngineAssetsDir"_s, u""_s);
            backend->answerJson(u"ListItems"_s, items);
            backend->answerJson(u"Assignments"_s, assignments);
            backend->answerJson(u"Settings"_s, "{}");
            backend->answerJson(u"ListPlaylists"_s, "[]");
            backend->answerJson(u"ListPets"_s, "[]");
            backend->answer(u"SetAssignment"_s, QVariant());
            backend->answer(u"ClearAssignment"_s, QVariant());
            backend->setAvailable(true);
            QTRY_COMPARE(client.assignments().size(), QJsonDocument::fromJson(assignments).array().size());
            QTRY_COMPARE(library.rowCount(), QJsonDocument::fromJson(items).array().size());
        }
        // The daemon changed its assignments and says so.
        void daemonNowHas(const QByteArray &assignments)
        {
            backend->answerJson(u"Assignments"_s, assignments);
            backend->emitSignal(u"AssignmentsChanged"_s);
        }
        QStringList methods() const
        {
            QStringList out;
            for (const auto &call : backend->calls) {
                if (call.first == u"SetAssignment"_s || call.first == u"ClearAssignment"_s)
                    out << call.first + u'(' + call.second.value(0).toString() + u'/' + call.second.value(1).toString() + u')';
            }
            return out;
        }
    };

private slots:
    void appliesAndConfirmsOnceTheDaemonHasIt()
    {
        Rig rig;
        QSignalSpy states(&rig.flow, &UseNowFlow::stateChanged);
        rig.flow.start(u"we:1"_s, true);
        QCOMPARE(rig.flow.state(), u"applying"_s);
        QCOMPARE(rig.methods(), (QStringList{u"SetAssignment(*/*)"_s}));
        QCOMPARE(rig.backend->calls.last().second.at(2).toString(), u"we:1"_s);
        QTest::qWait(50);
        QCOMPARE(rig.flow.state(), u"applying"_s); // nothing is said before the daemon agrees
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:1"}])");
        QTRY_COMPARE(rig.flow.state(), u"applied"_s);
        rig.flow.reset();
        QCOMPARE(rig.flow.state(), u"idle"_s);
    }

    void anAlreadyAppliedItemIsConfirmedAtOnce()
    {
        Rig rig(R"([{"id":"we:1","title":"One"}])", R"([{"output":"*","desktop":"*","itemId":"we:1"}])");
        rig.flow.start(u"we:1"_s, true);
        QCOMPARE(rig.flow.state(), u"applied"_s);
        QCOMPARE(rig.methods(), QStringList()); // nothing to send
    }

    void replacingClearsTheScreensFirstAndConfirmsWhenTheyAreGone()
    {
        Rig rig(R"([{"id":"we:1","title":"One"}])",
                R"([{"output":"DP-1","desktop":"*","itemId":"we:2"},{"output":"HDMI-A-1","desktop":"*","itemId":"we:3"}])");
        rig.flow.start(u"we:1"_s, true);
        QCOMPARE(rig.methods(), (QStringList{u"ClearAssignment(DP-1/*)"_s, u"ClearAssignment(HDMI-A-1/*)"_s, u"SetAssignment(*/*)"_s}));
        // the wildcard arrives but a screen is still set: not confirmed yet
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:1"},{"output":"DP-1","desktop":"*","itemId":"we:2"}])");
        QTest::qWait(50);
        QCOMPARE(rig.flow.state(), u"applying"_s);
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:1"}])");
        QTRY_COMPARE(rig.flow.state(), u"applied"_s);
    }

    void vanishedDesktopsAreClearedToo()
    {
        Rig rig(R"([{"id":"we:1","title":"One"}])", R"([{"output":"DP-1","desktop":"gone","itemId":"we:2"}])");
        rig.flow.start(u"we:1"_s, true, {u"d1"_s});
        QCOMPARE(rig.methods(), (QStringList{u"ClearAssignment(DP-1/gone)"_s, u"SetAssignment(*/*)"_s}));
    }

    void keepingTheScreensOnlySetsTheWildcard()
    {
        Rig rig(R"([{"id":"we:1","title":"One"}])", R"([{"output":"DP-1","desktop":"*","itemId":"we:2"}])");
        rig.flow.start(u"we:1"_s, false);
        QCOMPARE(rig.methods(), (QStringList{u"SetAssignment(*/*)"_s}));
        // the screen keeps its own wallpaper and that is fine: the wildcard is what was asked for
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:1"},{"output":"DP-1","desktop":"*","itemId":"we:2"}])");
        QTRY_COMPARE(rig.flow.state(), u"applied"_s);
    }

    void aDaemonErrorIsShownNotHidden()
    {
        Rig rig;
        rig.backend->fail(u"SetAssignment"_s, u"org.freedesktop.DBus.Error.InvalidArgs"_s, u"Unknown item: we:1"_s);
        rig.flow.start(u"we:1"_s, true);
        QTRY_COMPARE(rig.flow.state(), u"failed"_s);
        QVERIFY(rig.flow.message().contains(u"Unknown item"_s));
    }

    void silenceIsAFailureToo()
    {
        Rig rig;
        rig.flow.setTimeouts(30000, 150);
        rig.flow.start(u"we:1"_s, true);
        QTRY_COMPARE(rig.flow.state(), u"failed"_s);
        QVERIFY(rig.flow.message().contains(u"confirm"_s));
    }

    void waitsForAnItemSteamHasJustFinished()
    {
        Rig rig(R"([{"id":"we:9","title":"Nine"}])");
        rig.flow.start(u"we:1"_s, true);
        QCOMPARE(rig.flow.state(), u"waiting"_s);
        QCOMPARE(rig.methods(), QStringList()); // nothing is sent for an item the daemon does not know
        QTest::qWait(100);
        QCOMPARE(rig.flow.state(), u"waiting"_s);
        // the watcher catches up
        rig.backend->answerJson(u"ListItems"_s, R"([{"id":"we:9","title":"Nine"},{"id":"we:1","title":"One"}])");
        rig.backend->emitSignal(u"ItemsChanged"_s, {QStringList{u"we:1"_s}, QStringList(), QStringList()});
        QTRY_COMPARE(rig.flow.state(), u"applying"_s);
        QCOMPARE(rig.methods(), (QStringList{u"SetAssignment(*/*)"_s}));
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:1"}])");
        QTRY_COMPARE(rig.flow.state(), u"applied"_s);
    }

    void givesUpWaitingAfterTheLimit()
    {
        Rig rig;
        rig.flow.setTimeouts(150, 5000);
        rig.flow.start(u"we:404"_s, true);
        QCOMPARE(rig.flow.state(), u"waiting"_s);
        QTRY_COMPARE(rig.flow.state(), u"failed"_s);
        QVERIFY(rig.flow.message().contains(u"Steam"_s));
        QCOMPARE(rig.methods(), QStringList());
    }

    void withoutTheServiceItFailsAtOnce()
    {
        FakeBackend *backend = new FakeBackend;
        DaemonClient client(backend);
        LibraryModel library(&client);
        UseNowFlow flow(&client, &library);
        flow.start(u"we:1"_s, true);
        QCOMPARE(flow.state(), u"failed"_s);
        QVERIFY(flow.message().contains(u"not running"_s));
    }

    void aSecondStartReplacesTheFirst()
    {
        Rig rig(R"([{"id":"we:1","title":"One"},{"id":"we:2","title":"Two"}])");
        rig.flow.start(u"we:1"_s, true);
        rig.flow.start(u"we:2"_s, true);
        QCOMPARE(rig.flow.itemId(), u"we:2"_s);
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:1"}])"); // the first one landing is not the answer
        QTest::qWait(50);
        QCOMPARE(rig.flow.state(), u"applying"_s);
        rig.daemonNowHas(R"([{"output":"*","desktop":"*","itemId":"we:2"}])");
        QTRY_COMPARE(rig.flow.state(), u"applied"_s);
    }
};

QTEST_MAIN(TstUseNowFlow)
#include "tst_usenowflow.moc"
