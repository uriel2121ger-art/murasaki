// The PipeWire backend against a running PipeWire server. Manual: it is skipped unless MURASAKI_TEST_PIPEWIRE=1, because it
// opens streams on the session's real audio graph (a silent playback stream and a capture stream of the default output's
// monitor, a second each). Nothing audible is played.

#include "audio/backend.h"

#include <QElapsedTimer>
#include <QTest>

#include <atomic>

using namespace murasaki::scene;

class TstSceneVkPipeWire : public QObject
{
    Q_OBJECT

private slots:
    void streamsDeliverAndRequestFrames()
    {
        if (qEnvironmentVariable("MURASAKI_TEST_PIPEWIRE") != "1")
            QSKIP("set MURASAKI_TEST_PIPEWIRE=1 to run against the session's PipeWire");
        auto backend = makePipeWireBackend();
        if (!backend)
            QSKIP("built without PipeWire");
        std::atomic<size_t> pulled{0}, pushed{0};
        std::string error;
        QVERIFY2(backend->startPlayback([&](float *out, size_t frames) {
                     std::fill(out, out + frames * 2, 0.0f); // silence
                     pulled += frames;
                 }, &error),
                 error.c_str());
        QVERIFY2(backend->startCapture([&](const float *, size_t frames) { pushed += frames; }, &error), error.c_str());
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000 && (pulled < 48000 / 2 || pushed < 48000 / 4))
            QTest::qWait(50);
        backend->stopCapture();
        backend->stopPlayback();
        QVERIFY2(pulled > 48000 / 4, qPrintable(QString("pulled %1").arg(pulled.load())));
        QVERIFY2(pushed > 0, qPrintable(QString("pushed %1").arg(pushed.load())));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkPipeWire)
#include "tst_scenevk_pipewire.moc"
