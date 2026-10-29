// Tests of the shared Murasaki.Bridge module: it is imported from the build directory (the way the Plasma
// package imports it from its own directory), BridgeItem, MurasakiStatus and MurasakiOutputStatus are
// instantiated, nothing is logged as a warning, and the status types talk to a fake daemon on the private test
// bus. Runs offscreen.
#include "constants.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <memory>

namespace {

QStringList g_warnings;
QtMessageHandler g_previous = nullptr;

void collect(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        g_warnings << message;
    if (g_previous)
        g_previous(type, context, message);
}

constexpr char kQml[] = R"(
import QtQuick
import Murasaki.Bridge

Item {
    width: 200; height: 100
    property alias bridge: host
    property alias status: out
    property string label: out.kind + ":" + out.itemId
    function setDesktop(desktop) { MurasakiStatus.desktop = desktop }
    function desktop() { return MurasakiStatus.desktop }
    MurasakiOutputStatus { id: out; output: "DP-1" }
    BridgeItem {
        id: host
        anchors.fill: parent
        program: "/bin/true"
        arguments: ["--example"]
        fill: "fit"
        fps: 20
        paused: true
        volume: 0.5
        properties: ({ speed: 1.5, tint: [1, 0, 0] })
    }
}
)";

QString itemReply(const QString &id, const QString &kind, const QString &preview)
{
    const QJsonObject item{{"id", id}, {"kind", kind}, {"preview", preview}};
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"assignment", QJsonObject{{"itemId", id}}}, {"item", item}})
                                 .toJson(QJsonDocument::Compact));
}

// Stands in for murasakid on the private bus: ResolveAssignment answers from a table keyed "output/desktop"
// (or holds the call until the test releases it), and the change signals can be emitted on demand.
class FakeDaemon : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki1")
    Q_PROPERTY(uint ApiVersion MEMBER apiVersion CONSTANT)

public:
    uint apiVersion = murasaki::kMinDBusApiVersion;
    struct Call {
        QString output;
        QString desktop;
        QDBusMessage message;
    };

    QHash<QString, QString> table; // "DP-1/*" -> reply JSON
    bool hold = false;
    QList<Call> calls; // every call received, in order
    QList<Call> held;

    void reset()
    {
        table.clear();
        hold = false;
        apiVersion = murasaki::kMinDBusApiVersion;
        calls.clear();
        held.clear();
    }
    // A connection of its own, so the client's calls go through the bus daemon like they do to the real one
    // (a call on the process's own connection would be short-circuited and could not be answered late).
    bool start()
    {
        auto bus = connection();
        return bus.registerObject(QString(murasaki::kDBusPath), this,
                                  QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals
                                      | QDBusConnection::ExportAllProperties)
            && bus.registerService(QString(murasaki::kDBusService));
    }
    QDBusConnection connection()
    {
        if (!m_connection.isConnected())
            m_connection = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-daemon"));
        return m_connection;
    }
    void stop()
    {
        auto bus = connection();
        bus.unregisterService(QString(murasaki::kDBusService));
        bus.unregisterObject(QString(murasaki::kDBusPath));
    }
    void answer(const Call &call)
    {
        connection().send(
            call.message.createReply(table.value(call.output + '/' + call.desktop, itemReply({}, {}, {}))));
    }

private:
    QDBusConnection m_connection{QStringLiteral("unconnected")};

public slots:
    void ResolveAssignment(const QString &output, const QString &desktop, const QDBusMessage &message)
    {
        message.setDelayedReply(true);
        const Call call{output, desktop, message};
        calls << call;
        if (hold)
            held << call;
        else
            answer(call);
    }

signals:
    void AssignmentsChanged();
    void ItemsChanged(const QStringList &added, const QStringList &changed, const QStringList &removed);
};

} // namespace

class TstBridgeQml : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_root;
    FakeDaemon m_daemon;

    QObject *load()
    {
        QQmlComponent component(&m_engine);
        component.setData(kQml, QUrl());
        m_root.reset(component.create());
        if (!m_root)
            qWarning("%s", qPrintable(component.errorString()));
        return m_root.get();
    }
    QUrl preview() const { return m_root->property("status").value<QObject *>()->property("preview").toUrl(); }
    QString label() const { return m_root->property("label").toString(); }

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
        // The software backend is not OpenGL: BridgeItem must refuse it instead of launching anything.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        g_previous = qInstallMessageHandler(collect);
        m_engine.addImportPath(QStringLiteral(MURASAKI_BRIDGE_QML_DIR));
    }
    void cleanupTestCase()
    {
        qInstallMessageHandler(g_previous);
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-daemon"));
    }
    void init()
    {
        g_warnings.clear();
        m_daemon.reset();
    }
    void cleanup()
    {
        if (m_root)
            QMetaObject::invokeMethod(m_root.get(), "setDesktop", Q_ARG(QVariant, QStringLiteral("*")));
        m_root.reset();
        m_daemon.stop();
    }

    void loadsFromBuildDirectory()
    {
        QVERIFY(load());
        auto *host = m_root->property("bridge").value<QQuickItem *>();
        QVERIFY(host);
        QCOMPARE(host->property("program").toString(), QStringLiteral("/bin/true"));
        QCOMPARE(host->property("arguments").toStringList(), QStringList{QStringLiteral("--example")});
        QCOMPARE(host->property("fill").toString(), QStringLiteral("fit"));
        QCOMPARE(host->property("fps").toInt(), 20);
        QCOMPARE(host->property("paused").toBool(), true);
        QCOMPARE(host->property("hasFrame").toBool(), false);
        QCOMPARE(host->property("properties").toMap().value(QStringLiteral("speed")).toDouble(), 1.5);
        QCOMPARE(m_root->property("status").value<QObject *>()->property("output").toString(), QStringLiteral("DP-1"));
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join('\n')));
    }

    void noDaemonMeansNoPreviewAndNoAutostart()
    {
        QVERIFY(load());
        QTest::qWait(300);
        QCOMPARE(preview(), QUrl());
        QCOMPARE(label(), QStringLiteral(":"));
        // The user's own daemon has a D-Bus service file; asking about a preview must not launch it.
        QVERIFY(!QDBusConnection::sessionBus().interface()->isServiceRegistered(QString(murasaki::kDBusService)));
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join('\n')));
    }

    void replyIsAppliedAndBindingsFollow()
    {
        m_daemon.table["DP-1/*"] = itemReply("item1", "video", "/tmp/previews/one.png");
        QVERIFY(m_daemon.start());
        QVERIFY(load());
        QTRY_COMPARE(preview(), QUrl::fromLocalFile("/tmp/previews/one.png"));
        QCOMPARE(m_root->property("status").value<QObject *>()->property("itemId").toString(), QStringLiteral("item1"));
        QCOMPARE(label(), QStringLiteral("video:item1")); // a binding on kind and itemId re-evaluated
        QVERIFY(!m_daemon.calls.isEmpty()); // a late registration notice may add a second, harmless lookup
        for (const auto &call : std::as_const(m_daemon.calls)) {
            QCOMPARE(call.output, QStringLiteral("DP-1"));
            QCOMPARE(call.desktop, QStringLiteral("*"));
        }
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join('\n')));
    }

    void oldDaemonIsUnavailable()
    {
        m_daemon.apiVersion = murasaki::kMinDBusApiVersion - 1;
        m_daemon.table["DP-1/*"] = itemReply("item1", "video", "/tmp/previews/one.png");
        QVERIFY(m_daemon.start());
        QVERIFY(load());
        QTest::qWait(300);
        QCOMPARE(preview(), QUrl());
        QCOMPARE(label(), QStringLiteral(":"));
        QVERIFY(m_daemon.calls.isEmpty()); // never asked
    }

    void staleReplyIsDropped()
    {
        m_daemon.hold = true;
        m_daemon.table["DP-1/*"] = itemReply("old", "web", "/tmp/previews/old.png");
        m_daemon.table["DP-1/desk2"] = itemReply("new", "scene", "/tmp/previews/new.png");
        QVERIFY(m_daemon.start());
        QVERIFY(load());
        QTRY_VERIFY(!m_daemon.held.isEmpty()); // the lookup for desktop "*" (a registration notice may add one)
        QMetaObject::invokeMethod(m_root.get(), "setDesktop", Q_ARG(QVariant, QStringLiteral("desk2")));
        const auto superseding = [this] {
            return std::any_of(m_daemon.held.cbegin(), m_daemon.held.cend(),
                               [](const auto &c) { return c.desktop == QStringLiteral("desk2"); });
        };
        QTRY_VERIFY(superseding());
        const auto held = m_daemon.held;
        for (const auto &call : held) {
            if (call.desktop == QStringLiteral("desk2"))
                m_daemon.answer(call);
        }
        QTRY_COMPARE(label(), QStringLiteral("scene:new"));
        for (const auto &call : held) { // late answers to the superseded lookups
            if (call.desktop != QStringLiteral("desk2"))
                m_daemon.answer(call);
        }
        QTest::qWait(200);
        QCOMPARE(label(), QStringLiteral("scene:new"));
        QCOMPARE(preview(), QUrl::fromLocalFile("/tmp/previews/new.png"));
    }

    void resolvesAgainOnDaemonSignalsAndDesktopChange()
    {
        m_daemon.table["DP-1/*"] = itemReply("a", "video", "/tmp/previews/a.png");
        QVERIFY(m_daemon.start());
        QVERIFY(load());
        QTRY_COMPARE(label(), QStringLiteral("video:a"));

        m_daemon.table["DP-1/*"] = itemReply("b", "web", "/tmp/previews/b.png");
        emit m_daemon.AssignmentsChanged();
        QTRY_COMPARE(label(), QStringLiteral("web:b"));

        m_daemon.table["DP-1/*"] = itemReply("c", "scene", "/tmp/previews/c.png");
        emit m_daemon.ItemsChanged({}, {QStringLiteral("c")}, {});
        QTRY_COMPARE(label(), QStringLiteral("scene:c"));

        m_daemon.table["DP-1/desk3"] = itemReply("d", "video", "/tmp/previews/d.png");
        QMetaObject::invokeMethod(m_root.get(), "setDesktop", Q_ARG(QVariant, QStringLiteral("desk3")));
        QTRY_COMPARE(label(), QStringLiteral("video:d"));
        QCOMPARE(m_daemon.calls.last().desktop, QStringLiteral("desk3"));
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join('\n')));
    }

    void retriesWhenTheDaemonRegisters()
    {
        QVERIFY(load());
        QTest::qWait(200);
        QCOMPARE(preview(), QUrl()); // nobody is there yet
        m_daemon.table["DP-1/*"] = itemReply("late", "video", "/tmp/previews/late.png");
        QVERIFY(m_daemon.start());
        QTRY_COMPARE(label(), QStringLiteral("video:late"));
    }

    void refusesANonOpenGlBackend()
    {
        QVERIFY(load());
        auto *item = qobject_cast<QQuickItem *>(m_root.get());
        auto *host = m_root->property("bridge").value<QQuickItem *>();
        QVERIFY(item && host);

        QQuickWindow window;
        window.resize(200, 100);
        item->setParentItem(window.contentItem());
        QSignalSpy failed(host, SIGNAL(failed(QString)));
        window.show();
        QVERIFY(QTest::qWaitFor([&] { return failed.count() > 0; }, 5000));
        QCOMPARE(failed.first().first().toString(), QStringLiteral("unsupported graphics backend"));
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join('\n')));
    }
};

QTEST_MAIN(TstBridgeQml)
#include "tst_bridgeqml.moc"
