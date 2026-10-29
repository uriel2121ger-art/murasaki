// ForeignToplevelMonitor against a fake compositor: a real Wayland server (libwayland-server, in a thread of this test)
// that offers wl_output and zwlr_foreign_toplevel_manager_v1 and plays the events a compositor sends. No real
// compositor and no display is involved; the socket lives in a temporary XDG_RUNTIME_DIR.

#include "foreigntoplevelmonitor.h"
#include "wlr-foreign-toplevel-management-unstable-v1-server-protocol.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <wayland-server.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// The fake compositor. Everything on the wayland objects happens on its own thread; the test queues work with run().
class FakeCompositor
{
public:
    explicit FakeCompositor(const QString &socket, bool withManager = true)
    {
        m_display = wl_display_create();
        wl_display_add_socket(m_display, socket.toUtf8().constData());
        if (withManager)
            wl_global_create(m_display, &zwlr_foreign_toplevel_manager_v1_interface, 3, this, &FakeCompositor::bindManager);
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
    void addOutput(const char *name) // before a client connects: each output is a global of its own
    {
        m_outputNames.push_back(std::make_unique<Named>(Named{this, name}));
        wl_global_create(m_display, &wl_output_interface, 4, m_outputNames.back().get(), &FakeCompositor::bindOutput);
    }
    wl_resource *newWindow(const char *appId, std::initializer_list<uint32_t> states, const char *output)
    {
        wl_resource *handle = wl_resource_create(wl_resource_get_client(m_manager), &zwlr_foreign_toplevel_handle_v1_interface,
                                                 wl_resource_get_version(m_manager), 0);
        wl_resource_set_implementation(handle, &kHandleImpl, nullptr, nullptr);
        zwlr_foreign_toplevel_manager_v1_send_toplevel(m_manager, handle);
        zwlr_foreign_toplevel_handle_v1_send_title(handle, "a window");
        zwlr_foreign_toplevel_handle_v1_send_app_id(handle, appId);
        if (output)
            zwlr_foreign_toplevel_handle_v1_send_output_enter(handle, m_outputs.at(output));
        sendState(handle, states);
        zwlr_foreign_toplevel_handle_v1_send_done(handle);
        return handle;
    }
    void setState(wl_resource *handle, std::initializer_list<uint32_t> states)
    {
        sendState(handle, states);
        zwlr_foreign_toplevel_handle_v1_send_done(handle);
    }
    void moveWindow(wl_resource *handle, const char *from, const char *to)
    {
        zwlr_foreign_toplevel_handle_v1_send_output_leave(handle, m_outputs.at(from));
        zwlr_foreign_toplevel_handle_v1_send_output_enter(handle, m_outputs.at(to));
        zwlr_foreign_toplevel_handle_v1_send_done(handle);
    }
    void closeWindow(wl_resource *handle) { zwlr_foreign_toplevel_handle_v1_send_closed(handle); }
    void finish() { zwlr_foreign_toplevel_manager_v1_send_finished(m_manager); }

private:
    static void sendState(wl_resource *handle, std::initializer_list<uint32_t> states)
    {
        wl_array array;
        wl_array_init(&array);
        for (const uint32_t state : states)
            *static_cast<uint32_t *>(wl_array_add(&array, sizeof(uint32_t))) = state;
        zwlr_foreign_toplevel_handle_v1_send_state(handle, &array);
        wl_array_release(&array);
    }

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

    static void outputRelease(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }
    static inline const struct wl_output_interface kOutputImpl = {outputRelease};

    static void bindOutput(wl_client *client, void *data, uint32_t version, uint32_t id)
    {
        auto *named = static_cast<Named *>(data);
        FakeCompositor *self = named->owner;
        wl_resource *resource = wl_resource_create(client, &wl_output_interface, int(version), id);
        wl_resource_set_implementation(resource, &kOutputImpl, nullptr, nullptr);
        const std::string name = named->name;
        self->m_outputs[name] = resource;
        wl_output_send_name(resource, name.c_str());
        wl_output_send_done(resource);
    }

    static void managerStop(wl_client *, wl_resource *resource)
    {
        zwlr_foreign_toplevel_manager_v1_send_finished(resource);
    }
    static inline const struct zwlr_foreign_toplevel_manager_v1_interface kManagerImpl = {managerStop};

    static void bindManager(wl_client *client, void *data, uint32_t version, uint32_t id)
    {
        auto *self = static_cast<FakeCompositor *>(data);
        self->m_manager = wl_resource_create(client, &zwlr_foreign_toplevel_manager_v1_interface, int(version), id);
        wl_resource_set_implementation(self->m_manager, &kManagerImpl, nullptr, nullptr);
    }

    static void noop(wl_client *, wl_resource *) {}
    static void noopOutput(wl_client *, wl_resource *, wl_resource *) {}
    static void destroyHandle(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }
    static void rectangle(wl_client *, wl_resource *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
    static void fullscreen(wl_client *, wl_resource *, wl_resource *) {}
    static inline const struct zwlr_foreign_toplevel_handle_v1_interface kHandleImpl = {
        noop, noop, noop, noop, noopOutput, noop, rectangle, destroyHandle, fullscreen, noop};

    wl_display *m_display = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{true};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_queue;
    struct Named {
        FakeCompositor *owner;
        std::string name;
    };
    std::vector<std::unique_ptr<Named>> m_outputNames;
    std::map<std::string, wl_resource *> m_outputs;
    wl_resource *m_manager = nullptr;
};

// A monitor whose state the test sets.
class ScriptedMonitor : public SessionMonitor
{
public:
    void set(const SessionState &state) { setState(state); }
};

constexpr uint32_t kMaximized = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED;
constexpr uint32_t kMinimized = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
constexpr uint32_t kActivated = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
constexpr uint32_t kFullscreen = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;

} // namespace

class TestForeignToplevelMonitor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void probeListsTheGlobals();
    void windowsThatExistAtStartAreKnown();
    void eventsMoveTheState();
    void minimizingAndClosingUncover();
    void aWindowMovedToAnotherOutputMovesTheCover();
    void appsOnlyLeavesOcclusionToThePrimaryMonitor();
    void aCompositorWithoutTheProtocolLeavesTheStateAlone();
    void theCompositorStoppingTheProtocolClearsTheState();
    void mergedMonitorTakesOcclusionFromTheFirstAndAppsFromTheSecond();

private:
    QString socket() { return u"murasaki-test-%1-%2"_s.arg(QCoreApplication::applicationPid()).arg(++m_serial); }
    QTemporaryDir m_runtime;
    int m_serial = 0;
};

void TestForeignToplevelMonitor::initTestCase()
{
    QVERIFY(m_runtime.isValid());
    QFile::setPermissions(m_runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", m_runtime.path().toUtf8());
    qunsetenv("WAYLAND_DISPLAY");
}

void TestForeignToplevelMonitor::probeListsTheGlobals()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    const QSet<QString> globals = probeWaylandGlobals(name);
    QVERIFY(globals.contains(u"wl_output"_s));
    QVERIFY(globals.contains(u"zwlr_foreign_toplevel_manager_v1"_s));
    QVERIFY(!globals.contains(u"zwlr_layer_shell_v1"_s));
    QVERIFY(probeWaylandGlobals(u"murasaki-nobody-home"_s).isEmpty());
}

void TestForeignToplevelMonitor::windowsThatExistAtStartAreKnown()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    compositor.addOutput("HDMI-A-1");
    ForeignToplevelMonitor monitor(name);
    // The compositor sends a window as soon as the manager is bound: queue it from its thread right after.
    monitor.start();
    QVERIFY(monitor.connected());
    const SessionState empty = monitor.state();
    QVERIFY(empty.outputs.contains(u"DP-1"_s) && empty.outputs.contains(u"HDMI-A-1"_s));
    QVERIFY(!empty.outputs.value(u"DP-1"_s).occluded);
}

void TestForeignToplevelMonitor::eventsMoveTheState()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    compositor.addOutput("HDMI-A-1");
    ForeignToplevelMonitor monitor(name);
    monitor.start();
    QSignalSpy spy(&monitor, &SessionMonitor::stateChanged);

    wl_resource *mpv = nullptr;
    compositor.run([&] { mpv = compositor.newWindow("mpv", {kFullscreen, kActivated}, "DP-1"); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    QVERIFY(!monitor.state().outputs.value(u"HDMI-A-1"_s).occluded);
    QCOMPARE(monitor.state().activeApp, u"mpv"_s);
    QCOMPARE(monitor.state().fullscreenApps, QStringList{u"mpv"_s});
    QVERIFY(spy.count() >= 1);

    compositor.run([&] { compositor.setState(mpv, {kActivated}); }); // leaves fullscreen
    QVERIFY(QTest::qWaitFor([&] { return !monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    QVERIFY(monitor.state().fullscreenApps.isEmpty());

    compositor.run([&] { compositor.setState(mpv, {kMaximized, kActivated}); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
}

void TestForeignToplevelMonitor::minimizingAndClosingUncover()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    ForeignToplevelMonitor monitor(name);
    monitor.start();
    wl_resource *window = nullptr;
    compositor.run([&] { window = compositor.newWindow("editor", {kMaximized}, "DP-1"); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    compositor.run([&] { compositor.setState(window, {kMaximized, kMinimized}); });
    QVERIFY(QTest::qWaitFor([&] { return !monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    compositor.run([&] { compositor.setState(window, {kMaximized}); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    compositor.run([&] { compositor.closeWindow(window); });
    QVERIFY(QTest::qWaitFor([&] { return !monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
}

void TestForeignToplevelMonitor::aWindowMovedToAnotherOutputMovesTheCover()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    compositor.addOutput("HDMI-A-1");
    ForeignToplevelMonitor monitor(name);
    monitor.start();
    wl_resource *window = nullptr;
    compositor.run([&] { window = compositor.newWindow("mpv", {kFullscreen}, "DP-1"); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    compositor.run([&] { compositor.moveWindow(window, "DP-1", "HDMI-A-1"); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"HDMI-A-1"_s).occluded; }, 5000));
    QVERIFY(!monitor.state().outputs.value(u"DP-1"_s).occluded);
}

void TestForeignToplevelMonitor::appsOnlyLeavesOcclusionToThePrimaryMonitor()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    ForeignToplevelMonitor monitor(name, /*appsOnly=*/true);
    monitor.start();
    compositor.run([&] { compositor.newWindow("steam_app_620", {kFullscreen, kActivated}, "DP-1"); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().activeApp == u"steam_app_620"_s; }, 5000));
    QVERIFY(monitor.state().outputs.isEmpty());
    QCOMPARE(monitor.state().fullscreenApps, QStringList{u"steam_app_620"_s});
}

void TestForeignToplevelMonitor::aCompositorWithoutTheProtocolLeavesTheStateAlone()
{
    const QString name = socket();
    FakeCompositor compositor(name, /*withManager=*/false);
    compositor.addOutput("DP-1");
    ForeignToplevelMonitor monitor(name);
    monitor.start();
    QCOMPARE(monitor.state(), SessionState{});
}

void TestForeignToplevelMonitor::theCompositorStoppingTheProtocolClearsTheState()
{
    const QString name = socket();
    FakeCompositor compositor(name);
    compositor.addOutput("DP-1");
    ForeignToplevelMonitor monitor(name);
    monitor.start();
    compositor.run([&] { compositor.newWindow("mpv", {kFullscreen}, "DP-1"); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state().outputs.value(u"DP-1"_s).occluded; }, 5000));
    compositor.run([&] { compositor.finish(); });
    QVERIFY(QTest::qWaitFor([&] { return monitor.state() == SessionState{}; }, 5000)); // wallpapers play again
}

void TestForeignToplevelMonitor::mergedMonitorTakesOcclusionFromTheFirstAndAppsFromTheSecond()
{
    auto primary = std::make_unique<ScriptedMonitor>();
    auto apps = std::make_unique<ScriptedMonitor>();
    ScriptedMonitor *primaryRaw = primary.get();
    ScriptedMonitor *appsRaw = apps.get();
    MergedMonitor merged(std::move(primary), std::move(apps));
    QSignalSpy spy(&merged, &SessionMonitor::stateChanged);

    SessionState occlusion;
    occlusion.outputs.insert(u"DP-1"_s, OutputSession{u"2"_s, true});
    occlusion.defaultDesktop = u"2"_s;
    primaryRaw->set(occlusion);
    SessionState names;
    names.activeApp = u"steam_app_620"_s;
    names.fullscreenApps = {u"steam_app_620"_s};
    names.outputs.insert(u"DP-1"_s, OutputSession{u"*"_s, false}); // ignored: not the apps' business
    appsRaw->set(names);

    const SessionState state = merged.state();
    QVERIFY(state.outputs.value(u"DP-1"_s).occluded);
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"2"_s);
    QCOMPARE(state.activeApp, u"steam_app_620"_s);
    QCOMPARE(state.fullscreenApps, QStringList{u"steam_app_620"_s});
    QVERIFY(spy.count() >= 1);
}

QTEST_GUILESS_MAIN(TestForeignToplevelMonitor)
#include "tst_foreigntoplevelmonitor.moc"
