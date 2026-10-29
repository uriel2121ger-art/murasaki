// The whole path of murasaki-screenmatch against a real PipeWire: a private PipeWire server (its own runtime directory, so nothing of the user's session is touched), a test producer of video
// frames, and the real helper behind the real sandbox profile, started by the real host with a descriptor connected to that server. Skipped where PipeWire's programs are not installed.
#include "crashledger.h"
#include "screenmatch/protocol.h"
#include "screenmatchhost.h"
#include "screenmatch/session.h"

#include <QDir>
#include <cstring>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace murasaki;
namespace sm = murasaki::screenmatch;

Q_DECLARE_METATYPE(murasaki::screenmatch::Reply)

namespace {

struct Box {
    int x, y, w, h, value;
};

QByteArray sceneGrid(const QList<Box> &boxes)
{
    QByteArray grey(200 * 120, char(20));
    for (const Box &b : boxes)
        for (int y = b.y; y < b.y + b.h; ++y)
            for (int x = b.x; x < b.x + b.w; ++x)
                grey[y * 200 + x] = char(b.value);
    GreyImage g;
    g.width = 200;
    g.height = 120;
    g.pixels = grey;
    return screenGridOf(g);
}

} // namespace

class TstScreenMatchPipeWire : public QObject
{
    Q_OBJECT
private:
    QProcessEnvironment m_env;
    QString m_pwLink, m_pwDump;

    QByteArray run(const QString &program, const QStringList &args)
    {
        QProcess p;
        p.setProcessEnvironment(m_env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(program, args);
        p.waitForFinished(5000);
        return p.readAllStandardOutput();
    }
    int nodeId(const QString &name)
    {
        const QJsonArray all = QJsonDocument::fromJson(run(m_pwDump, {})).array();
        for (const QJsonValue &v : all) {
            const QJsonObject o = v.toObject();
            if (o.value("type").toString() == u"PipeWire:Interface:Node"_s && o.value("info").toObject().value("props").toObject().value("node.name").toString() == name)
                return o.value("id").toInt();
        }
        return -1;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<murasaki::screenmatch::Reply>();
        m_pwLink = QStandardPaths::findExecutable(u"pw-link"_s);
        m_pwDump = QStandardPaths::findExecutable(u"pw-dump"_s);
        if (QStandardPaths::findExecutable(u"pipewire"_s).isEmpty() || m_pwLink.isEmpty() || m_pwDump.isEmpty())
            QSKIP("needs the pipewire server, pw-link and pw-dump");
    }

    void theHelperScoresRealPipeWireFramesBehindTheSandbox()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString runtime = dir.filePath(u"rt"_s);
        QVERIFY(QDir().mkpath(runtime));
        m_env = QProcessEnvironment::systemEnvironment();
        m_env.insert(u"PIPEWIRE_RUNTIME_DIR"_s, runtime);
        m_env.insert(u"XDG_RUNTIME_DIR"_s, runtime);
        m_env.insert(u"PIPEWIRE_CORE"_s, u"pw-test"_s);   // (the name the server listens on)
        m_env.insert(u"PIPEWIRE_REMOTE"_s, u"pw-test"_s); // (the name clients connect to)
        m_env.remove(u"DBUS_SESSION_BUS_ADDRESS"_s);

        QProcess server;
        server.setProcessEnvironment(m_env);
        server.start(QStandardPaths::findExecutable(u"pipewire"_s), {});
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(runtime + u"/pw-test"_s); }, 10000));
        QProcess producer;
        producer.setProcessEnvironment(m_env);
        producer.setProcessChannelMode(QProcess::ForwardedChannels);
        producer.start(QStringLiteral(MURASAKI_PWPRODUCER_PATH), {u"--switch-after"_s, u"60"_s});
        QVERIFY(QTest::qWaitFor([&] { return nodeId(u"murasaki-test-producer"_s) > 0; }, 15000));
        const int producerNode = nodeId(u"murasaki-test-producer"_s);

        // a descriptor connected to the private server, the way the portal's OpenPipeWireRemote gives one
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(fd >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const QByteArray path = (runtime + u"/pw-test"_s).toLocal8Bit();
        std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
        QVERIFY(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        ScreenMatchHost::Config config;
        config.helperPath = QStringLiteral(MURASAKI_SCREENMATCH_PATH);
        config.sandboxPath = QStringLiteral(MURASAKI_SANDBOX_PATH);
        config.requireSandbox = true;
        config.allowCapture = true;
        ScreenMatchHost host(config, &ledger);
        QSignalSpy replies(&host, &ScreenMatchHost::reply);
        QSignalSpy degraded(&host, &ScreenMatchHost::degraded);
        host.start(fd, {unsigned(producerNode)});
        QCOMPARE(host.state(), ScreenMatchHost::State::Running);
        QVERIFY2(QTest::qWaitFor([&] { return !replies.isEmpty() || host.state() != ScreenMatchHost::State::Running; }, 15000), "no hello");
        if (host.state() == ScreenMatchHost::State::Refused && !degraded.isEmpty() && degraded.last().at(0).toString() == u"sandbox unavailable"_s)
            QSKIP("the sandbox is not available here (needs Landlock)");
        QVERIFY2(host.state() == ScreenMatchHost::State::Running, qPrintable(degraded.isEmpty() ? u"helper ended"_s : degraded.last().at(0).toString()));
        QCOMPARE(replies.at(0).at(0).value<sm::Reply>().type, sm::ReplyType::Hello);

        const auto send = [&](const QByteArray &line) {
            const auto c = sm::parseCommand(line);
            QVERIFY2(c.has_value(), line.constData());
            host.send(*c);
        };
        send("trigger t structure");
        send("region t 0 0 0 0 200 120");
        sm::Command ref;
        ref.type = sm::CommandType::Ref;
        ref.trigger = u"t"_s;
        ref.grid = sceneGrid({{20, 15, 60, 40, 220}, {110, 60, 50, 45, 150}, {40, 80, 30, 25, 90}, {150, 10, 35, 30, 250}});
        host.send(ref);
        send("rate 8");
        send("start");

        // the helper's own stream appears as a node; linking it to the producer is what a session manager does in a real session
        QVERIFY2(QTest::qWaitFor([&] { return nodeId(u"murasaki-screenmatch"_s) > 0; }, 15000), "the helper never connected a stream to PipeWire");
        const QString outputs = QString::fromUtf8(run(m_pwLink, {u"-o"_s})), inputs = QString::fromUtf8(run(m_pwLink, {u"-i"_s}));
        QString out, in;
        for (const QString &l : outputs.split(u'\n'))
            if (l.trimmed().startsWith(u"murasaki-test-producer:"_s) && out.isEmpty())
                out = l.trimmed();
        for (const QString &l : inputs.split(u'\n'))
            if (l.trimmed().startsWith(u"murasaki-screenmatch:"_s) && in.isEmpty())
                in = l.trimmed();
        QVERIFY2(!out.isEmpty() && !in.isEmpty(), qPrintable(outputs + u"\n"_s + inputs));
        const QByteArray linked = run(m_pwLink, {u"-w"_s, out, in});
        if (!linked.isEmpty())
            qWarning().noquote() << "pw-link:" << out << in << linked;

        // frames flow: scene A scores 1000 against the reference of scene A, then the producer switches to scene B and the score drops
        const auto lastScore = [&]() {
            for (qsizetype i = replies.size() - 1; i >= 0; --i) {
                const auto r = replies.at(i).at(0).value<sm::Reply>();
                if (r.type == sm::ReplyType::Scores)
                    return r.permille.value(0, -2);
            }
            return -2;
        };
        const bool scored = QTest::qWaitFor([&] { return lastScore() == 1000; }, 20000);
        if (!scored) { // what the graph looked like, for whoever reads the failure
            QString nodes;
            for (const QJsonValue &v : QJsonDocument::fromJson(run(m_pwDump, {})).array()) {
                const QJsonObject o = v.toObject();
                if (o.value("type").toString() == u"PipeWire:Interface:Node"_s)
                    nodes += QStringLiteral("%1 %2 %3\n").arg(o.value("id").toInt()).arg(o.value("info").toObject().value("props").toObject().value("node.name").toString(), o.value("info").toObject().value("state").toString());
                if (o.value("type").toString() == u"PipeWire:Interface:Link"_s)
                    nodes += QStringLiteral("link %1\n").arg(o.value("info").toObject().value("state").toString());
            }
            qWarning().noquote() << "producer:" << producer.state() << producer.exitStatus() << producer.exitCode();
            qWarning().noquote() << "graph:\n" << nodes << "links:" << run(m_pwLink, {u"-l"_s});
        }
        QVERIFY2(scored, qPrintable(QString::number(lastScore())));
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Running; }, 100));
        bool watching = false;
        for (qsizetype i = 0; i < replies.size(); ++i) {
            const auto r = replies.at(i).at(0).value<sm::Reply>();
            watching = watching || (r.type == sm::ReplyType::State && r.word == u"watching"_s);
        }
        QVERIFY(watching);
        QVERIFY2(QTest::qWaitFor([&] { const int s = lastScore(); return s >= 0 && s < 700; }, 30000), qPrintable(QString::number(lastScore())));

        // an explicit capture gives the picture of the rectangle, and only then
        const qsizetype before = replies.size();
        send("capture 5 0 0 0 100 100");
        QVERIFY(QTest::qWaitFor([&] {
            for (qsizetype i = before; i < replies.size(); ++i)
                if (replies.at(i).at(0).value<sm::Reply>().type == sm::ReplyType::ShotEnd)
                    return true;
            return false;
        }, 10000));
        QByteArray rgb;
        for (qsizetype i = before; i < replies.size(); ++i) {
            const auto r = replies.at(i).at(0).value<sm::Reply>();
            if (r.type == sm::ReplyType::Chunk)
                rgb += r.data;
        }
        QCOMPARE(rgb.size(), 100 * 100 * 3);

        host.stop();
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Idle; }, 5000));
        QCOMPARE(host.crashes(), 0);
        producer.kill();
        producer.waitForFinished(3000);
        server.kill();
        server.waitForFinished(3000);
    }
};

QTEST_MAIN(TstScreenMatchPipeWire)
#include "tst_screenmatchpipewire.moc"
