// WaylandIdleMonitor against a fake compositor: a real Wayland server (libwayland-server, in a thread of this test) that offers wl_seat and ext_idle_notifier_v1 and plays
// idled and resumed. No real compositor and no display is involved; the socket lives in a temporary XDG_RUNTIME_DIR. What a real KWin does with the protocol is not tested here.

#include "ext-idle-notify-v1-server-protocol.h"
#include "waylandidlemonitor.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <wayland-server.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

class FakeCompositor
{
public:
    explicit FakeCompositor(const QString &socket, bool withNotifier = true, bool withSeat = true)
    {
        m_display = wl_display_create();
        wl_display_add_socket(m_display, socket.toUtf8().constData());
        if (withSeat)
            wl_global_create(m_display, &wl_seat_interface, 5, this, &FakeCompositor::bindSeat);
        if (withNotifier)
            wl_global_create(m_display, &ext_idle_notifier_v1_interface, 2, this, &FakeCompositor::bindNotifier);
        m_thread = std::thread([this] { loop(); });
    }
    ~FakeCompositor()
    {
        m_running = false;
        m_thread.join();
        wl_display_destroy_clients(m_display);
        wl_display_destroy(m_display);
    }

    // Runs `work` on the compositor's thread and waits for it.
    void run(std::function<void()> work)
    {
        std::unique_lock lock(m_mutex);
        bool done = false;
        m_queue.push([&] {
            work();
            std::lock_guard guard(m_mutex);
            done = true;
            m_cv.notify_all();
        });
        m_cv.wait(lock, [&] { return done; });
    }

    // ---- what a compositor does, called through run() ----
    void idled() { ext_idle_notification_v1_send_idled(m_notification); }
    void resumed() { ext_idle_notification_v1_send_resumed(m_notification); }
    void idleAtCreation(bool value) { m_idleAtCreation = value; }

    std::atomic<int> timeoutMs{-1};          // what get_idle_notification asked for
    std::atomic<int> idleRequests{0};        // get_idle_notification calls: the one that counts idle inhibitors
    std::atomic<int> inputIdleRequests{0};   // get_input_idle_notification calls: the one that does not

private:
    void loop()
    {
        wl_event_loop *events = wl_display_get_event_loop(m_display);
        while (m_running) {
            wl_event_loop_dispatch(events, 5);
            std::function<void()> job;
            {
                std::lock_guard lock(m_mutex);
                if (!m_queue.empty()) {
                    job = std::move(m_queue.front());
                    m_queue.pop();
                }
            }
            if (job)
                job();
            wl_display_flush_clients(m_display);
        }
    }

    static void bindSeat(wl_client *client, void *, uint32_t version, uint32_t id)
    {
        static const struct wl_seat_interface impl = {[](wl_client *, wl_resource *, uint32_t) {}, [](wl_client *, wl_resource *, uint32_t) {},
                                                      [](wl_client *, wl_resource *, uint32_t) {}, [](wl_client *, wl_resource *) {}};
        wl_resource *seat = wl_resource_create(client, &wl_seat_interface, int(version), id);
        wl_resource_set_implementation(seat, &impl, nullptr, nullptr);
        wl_seat_send_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    }

    static void destroyResource(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }
    static inline const struct ext_idle_notification_v1_interface kNotificationImpl = {destroyResource};

    void makeNotification(wl_client *client, wl_resource *notifier, uint32_t id, uint32_t timeout)
    {
        timeoutMs = int(timeout);
        m_notification = wl_resource_create(client, &ext_idle_notification_v1_interface, wl_resource_get_version(notifier), id);
        wl_resource_set_implementation(m_notification, &kNotificationImpl, nullptr, nullptr);
        if (m_idleAtCreation)
            ext_idle_notification_v1_send_idled(m_notification);
    }
    static void getIdle(wl_client *client, wl_resource *resource, uint32_t id, uint32_t timeout, wl_resource *)
    {
        auto *self = static_cast<FakeCompositor *>(wl_resource_get_user_data(resource));
        ++self->idleRequests;
        self->makeNotification(client, resource, id, timeout);
    }
    static void getInputIdle(wl_client *client, wl_resource *resource, uint32_t id, uint32_t timeout, wl_resource *)
    {
        auto *self = static_cast<FakeCompositor *>(wl_resource_get_user_data(resource));
        ++self->inputIdleRequests;
        self->makeNotification(client, resource, id, timeout);
    }
    static inline const struct ext_idle_notifier_v1_interface kNotifierImpl = {destroyResource, getIdle, getInputIdle};

    static void bindNotifier(wl_client *client, void *data, uint32_t version, uint32_t id)
    {
        wl_resource *resource = wl_resource_create(client, &ext_idle_notifier_v1_interface, int(version), id);
        wl_resource_set_implementation(resource, &kNotifierImpl, data, nullptr);
    }

    wl_display *m_display = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{true};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_queue;
    wl_resource *m_notification = nullptr;
    std::atomic<bool> m_idleAtCreation{false};
};

} // namespace

class TestWaylandIdleMonitor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void asksForTheTimeoutOnTheNotificationThatCountsInhibitors();
    void idledAndResumedMoveTheFact();
    void anIdleThatWasThereWhenItStartedIsKept();
    void aCompositorWithoutTheProtocolLeavesTheIdleUnknown();
    void aCompositorWithoutASeatLeavesTheIdleUnknown();
    void aMissingDisplayLeavesTheIdleUnknown();

private:
    QString socket() { return u"murasaki-idle-%1-%2"_s.arg(QCoreApplication::applicationPid()).arg(++m_serial); }
    QTemporaryDir m_runtime;
    int m_serial = 0;
};

void TestWaylandIdleMonitor::initTestCase()
{
    QVERIFY(m_runtime.isValid());
    QFile::setPermissions(m_runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", m_runtime.path().toUtf8());
    qunsetenv("WAYLAND_DISPLAY");
}

void TestWaylandIdleMonitor::asksForTheTimeoutOnTheNotificationThatCountsInhibitors()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    WaylandIdleMonitor monitor(60'000, name);
    QCOMPARE(monitor.afterMs(), 60'000);
    QVERIFY(!monitor.known());
    monitor.start();
    QVERIFY(monitor.known());
    QVERIFY(!monitor.idle()); // awake until the compositor says otherwise
    QCOMPARE(compositor.timeoutMs.load(), 60'000);
    QCOMPARE(compositor.idleRequests.load(), 1); // someone watching a video that inhibits idle is not away
    QCOMPARE(compositor.inputIdleRequests.load(), 0);
}

void TestWaylandIdleMonitor::idledAndResumedMoveTheFact()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    WaylandIdleMonitor monitor(5'000, name);
    monitor.start();
    QSignalSpy spy(&monitor, &IdleSource::changed);
    QCOMPARE(spy.count(), 0);
    compositor.run([&] { compositor.idled(); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.idle(); }, 5000));
    QVERIFY(monitor.known());
    compositor.run([&] { compositor.resumed(); });
    QVERIFY(QTest::qWaitFor([&] { return !monitor.idle(); }, 5000));
    QVERIFY(monitor.known());
    compositor.run([&] { compositor.idled(); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.idle(); }, 5000));
    QCOMPARE(spy.count(), 3);
}

void TestWaylandIdleMonitor::anIdleThatWasThereWhenItStartedIsKept()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.idleAtCreation(true); // the user was away for longer than the timeout when the notification was made
    WaylandIdleMonitor monitor(60'000, name);
    monitor.start();
    QVERIFY(monitor.known() && monitor.idle());
}

void TestWaylandIdleMonitor::aCompositorWithoutTheProtocolLeavesTheIdleUnknown()
{
    const QString name = socket();
    FakeCompositor compositor(name, /*withNotifier=*/false);
    WaylandIdleMonitor monitor(60'000, name);
    monitor.start();
    QVERIFY(!monitor.known() && !monitor.idle());
}

void TestWaylandIdleMonitor::aCompositorWithoutASeatLeavesTheIdleUnknown()
{
    const QString name = socket();
    FakeCompositor compositor(name, /*withNotifier=*/true, /*withSeat=*/false);
    WaylandIdleMonitor monitor(60'000, name);
    monitor.start();
    QVERIFY(!monitor.known());
    QCOMPARE(compositor.idleRequests.load(), 0);
}

void TestWaylandIdleMonitor::aMissingDisplayLeavesTheIdleUnknown()
{
    WaylandIdleMonitor monitor(60'000, u"murasaki-nobody-home"_s);
    monitor.start();
    QVERIFY(!monitor.known());
}

QTEST_MAIN(TestWaylandIdleMonitor)
#include "tst_waylandidlemonitor.moc"
