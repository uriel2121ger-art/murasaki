#include "fakebackend.h"
#include "overlayclient.h"

#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::OverlayClient;

class TstOverlayClient : public QObject
{
    Q_OBJECT

private slots:
    void readsTheShortcutBackendWhenTheOverlayAppears()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ShortcutBackend"_s, u"kglobalaccel"_s);
        OverlayClient client(backend);
        QSignalSpy available(&client, &OverlayClient::availableChanged);
        QSignalSpy shortcut(&client, &OverlayClient::shortcutBackendChanged);
        QVERIFY(!client.available());
        client.start();
        QCOMPARE(backend->startCount, 1);
        backend->setAvailable(true);
        QVERIFY(client.available());
        QTRY_COMPARE(client.shortcutBackend(), u"kglobalaccel"_s);
        QCOMPARE(shortcut.size(), 1);

        backend->setAvailable(false);
        QVERIFY(!client.available());
        QVERIFY(client.shortcutBackend().isEmpty());
        QCOMPARE(available.size(), 2);
        QCOMPARE(shortcut.size(), 2);
    }

    void neverAsksAnOverlayThatIsNotRunning()
    {
        // The overlay is D-Bus activatable: a property read would start it.
        auto *backend = new FakeBackend;
        backend->setProperty(u"ShortcutBackend"_s, u"portal"_s);
        OverlayClient client(backend);
        client.refresh();
        QTest::qWait(30);
        QVERIFY(client.shortcutBackend().isEmpty());
        backend->setAvailable(true);
        QTRY_COMPARE(client.shortcutBackend(), u"portal"_s);
    }

    void refreshPicksUpAChangeAndEmptyMeansNone()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ShortcutBackend"_s, u""_s);
        OverlayClient client(backend);
        backend->setAvailable(true);
        QTest::qWait(30);
        QVERIFY(client.shortcutBackend().isEmpty());
        backend->setProperty(u"ShortcutBackend"_s, u"portal"_s);
        client.refresh();
        QTRY_COMPARE(client.shortcutBackend(), u"portal"_s);
    }

    void windowsFollowTheOverlayAndRetryClearsABlock()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ShortcutBackend"_s, u"portal"_s);
        backend->answerJson(u"Windows"_s, R"([{"petId":"p1","shown":false,"failures":3,"blocked":true,"error":"bad file"}])");
        OverlayClient client(backend);
        QSignalSpy changed(&client, &OverlayClient::windowsChanged);
        QVERIFY(client.windows().isEmpty());
        backend->setAvailable(true);
        QTRY_COMPARE(client.windows().size(), 1);
        const QVariantMap window = client.windows().constFirst().toMap();
        QCOMPARE(window.value(u"petId"_s).toString(), u"p1"_s);
        QCOMPARE(window.value(u"blocked"_s).toBool(), true);
        QCOMPARE(window.value(u"error"_s).toString(), u"bad file"_s);

        // "Try again" asks the overlay, and the new state is read back.
        backend->answerJson(u"Windows"_s, R"([{"petId":"p1","shown":true,"failures":0,"blocked":false}])");
        backend->answer(u"ResetFailures"_s, QVariant());
        client.resetFailures(u"p1"_s);
        QTRY_COMPARE(backend->callCount(u"ResetFailures"_s), 1);
        QTRY_COMPARE(client.windows().constFirst().toMap().value(u"blocked"_s).toBool(), false);

        // The overlay announces a change: read again.
        backend->answerJson(u"Windows"_s, R"([])");
        backend->emitSignal(u"WindowsChanged"_s);
        QTRY_VERIFY(client.windows().isEmpty());

        backend->answerJson(u"Windows"_s, R"([{"petId":"p2"}])");
        backend->emitSignal(u"WindowsChanged"_s);
        QTRY_COMPARE(client.windows().size(), 1);
        backend->setAvailable(false); // nothing is known about a overlay that is gone
        QVERIFY(client.windows().isEmpty());
        QVERIFY(changed.size() >= 4);
    }

    void theSoundTestButtonCallsTheOverlayOnlyWhileItRuns()
    {
        auto *backend = new FakeBackend;
        backend->answerJson(u"Windows"_s, R"([])");
        backend->answer(u"PlayPetSound"_s, QVariant());
        backend->answer(u"StopPetSounds"_s, QVariant());
        OverlayClient client(backend);
        client.playPetSound(u"p1"_s); // not running: a call would start it
        client.stopPetSounds();
        QCOMPARE(backend->callCount(u"PlayPetSound"_s), 0);
        QCOMPARE(backend->callCount(u"StopPetSounds"_s), 0);
        backend->setAvailable(true);
        QTRY_VERIFY(client.available());
        client.playPetSound(u"p1"_s);
        client.stopPetSounds();
        QTRY_COMPARE(backend->callCount(u"PlayPetSound"_s), 1);
        QCOMPARE(backend->calls.last().second.size(), 0);
        QCOMPARE(backend->callCount(u"StopPetSounds"_s), 1);
    }

    void staleAnswerOfAReplacedOverlayIsDropped()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ShortcutBackend"_s, u"portal"_s);
        OverlayClient client(backend);
        backend->hold = true;
        backend->setAvailable(true);
        backend->setAvailable(false);
        backend->hold = false;
        backend->release();
        QTest::qWait(30);
        QVERIFY(client.shortcutBackend().isEmpty());
    }
};

QTEST_MAIN(TstOverlayClient)
#include "tst_overlayclient.moc"
