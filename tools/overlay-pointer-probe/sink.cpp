// Test tool (not installed): owns a D-Bus name and logs every call of ReportPointer(i, i) on the overlay's pointer interface, one line each:
// "report <ms since start> <x> <y>". It stands in for the overlay in tools/test-overlay-pointer-nested.sh, so that the real kwin-pet-cursor.js can be
// loaded into a nested KWin and its reports counted.
//
//   murasaki-pointer-sink NAME
#include <QCoreApplication>
#include <QDBusConnection>
#include <QElapsedTimer>
#include <QObject>

#include <cstdio>

class Sink : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1")

public:
    Sink() { m_clock.start(); }

public slots:
    Q_SCRIPTABLE void ReportPointer(int x, int y)
    {
        std::printf("report %lld %d %d\n", static_cast<long long>(m_clock.elapsed()), x, y);
        std::fflush(stdout);
    }

private:
    QElapsedTimer m_clock;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: murasaki-pointer-sink NAME\n");
        return 2;
    }
    Sink sink;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QStringLiteral("/io/github/uriel2121ger_art/Murasaki/Overlay/Pointer"), &sink, QDBusConnection::ExportScriptableSlots)
        || !bus.registerService(QString::fromLocal8Bit(argv[1]))) {
        std::fprintf(stderr, "murasaki-pointer-sink: cannot register on the session bus\n");
        return 1;
    }
    std::printf("ready\n");
    std::fflush(stdout);
    return app.exec();
}

#include "sink.moc"
