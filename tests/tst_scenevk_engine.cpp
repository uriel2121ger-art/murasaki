// murasaki-scene-vk as a process: the real binary against a fake bridge consumer on a socketpair. Covers the
// handshake, frame/release flow, pause, resize, transport switches and quit. Needs a Vulkan 1.3 device (skips
// without one). The scenes are tiny directories the test writes itself.

#include "bridge/channel.h"
#include "bridge/protocol.h"
#include "bridge/mediaview.h"
#include "bridge/shortcutview.h"
#include "bridge/spanview.h"
#include "core/context.h"
#include "engine/options.h"
#include "landlock.h"
#include "testutil.h"
#include "videofixtures.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <utility>
#include <string>
#include <vector>

using namespace Qt::StringLiterals;
using namespace murasaki;
namespace msg = murasaki::bridge::msg;
using murasaki::bridge::MsgType;

namespace {

struct Received {
    uint32_t type = 0;
    std::vector<uint8_t> payload;
    std::vector<bridge::UniqueFd> fds;
};

template <class T>
bool decodeAs(const Received &r, T &out)
{
    return r.type == static_cast<uint32_t>(T::kType) && bridge::decode(std::span<const uint8_t>(r.payload), out);
}

class Engine
{
public:
    ~Engine() { stop(); }

    bool start(const QString &sceneDir, const QString &size, bool forceShm, const QStringList &extra = {})
    {
        auto pair = bridge::makeSocketPair();
        if (!pair) {
            return false;
        }
        m_channel = bridge::Channel(std::move(pair->first));
        const int childFd = pair->second.get();
        std::vector<std::string> args = m_wrapper;
        args.insert(args.end(), {MURASAKI_SCENE_VK_PATH, "--murasaki-bridge-fd", std::to_string(childFd), "--murasaki-size",
                                         size.toStdString(), "--screen-root", "test", "--bg", sceneDir.toStdString(), "--fps", "60"});
        for (const QString &e : extra) {
            args.push_back(e.toStdString());
        }
        std::vector<char *> argv;
        for (auto &a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        m_pid = ::fork();
        if (m_pid < 0) {
            return false;
        }
        if (m_pid == 0) {
            const int flags = ::fcntl(childFd, F_GETFD);
            ::fcntl(childFd, F_SETFD, flags & ~FD_CLOEXEC);
            if (m_allowStill) {
                ::unsetenv("MURASAKI_SCENE_VK_REDRAW_ALWAYS");
            } else {
                ::setenv("MURASAKI_SCENE_VK_REDRAW_ALWAYS", "1", 1); // the tests count frames of still scenes
            }
            if (!m_log.empty()) {
                if (const int log = ::open(m_log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600); log >= 0) {
                    ::dup2(log, 2);
                }
            }
            if (forceShm) {
                ::setenv("MURASAKI_BRIDGE_FORCE_SHM", "1", 1);
            } else {
                ::unsetenv("MURASAKI_BRIDGE_FORCE_SHM");
            }
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        return true;
    }

    // Next message within the timeout, or nullopt.
    // (MediaWants, which the engine sends once its scene is loaded, is noted and not returned.)
    std::optional<Received> next(int timeoutMs)
    {
        for (;;) {
            pollfd p{m_channel.fd(), POLLIN, 0};
            if (::poll(&p, 1, timeoutMs) <= 0) {
                return std::nullopt;
            }
            bridge::ReceiveResult r = m_channel.receive();
            if (r.status != bridge::ChannelStatus::Ok) {
                m_closed = r.status == bridge::ChannelStatus::Closed;
                return std::nullopt;
            }
            if (r.message.type == static_cast<uint32_t>(bridge::kMediaWantsType)) {
                bridge::msg::MediaWants wants{};
                if (bridge::decode(r.message.payload, wants)) {
                    m_mediaWants = wants.wants != 0;
                }
                continue;
            }
            return Received{r.message.type, std::move(r.message.payload), std::move(r.message.fds)};
        }
    }
    // What the engine said about its scene using the media (MediaWants), once it did.
    std::optional<bool> mediaWants() const { return m_mediaWants; }

    template <class T>
    void send(const T &m)
    {
        QCOMPARE(m_channel.send(m, {}, bridge::SendMode::Blocking), bridge::ChannelStatus::Ok);
    }

    // A command that runs the engine (murasaki-sandbox and its options, ending in "--"), set before start().
    void wrapWith(std::vector<std::string> wrapper) { m_wrapper = std::move(wrapper); }
    // The engine's stderr goes to this file (its log: what it says about the item it loaded).
    void logTo(std::string path) { m_log = std::move(path); }
    pid_t pid() const { return m_pid; }
    // The engine may skip frames while nothing changes (the default of the real engine, off in the other tests).
    void allowStill() { m_allowStill = true; }

    bridge::Channel &channel() { return m_channel; }
    bool closed() const { return m_closed; }
    std::optional<bool> m_mediaWants;

    // Waits for the process; returns its exit status or -1 on timeout (the process is killed then).
    int wait(int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            int status = 0;
            if (::waitpid(m_pid, &status, WNOHANG) == m_pid) {
                m_pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            QTest::qSleep(10);
        }
        stop();
        return -1;
    }

    void stop()
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, nullptr, 0);
            m_pid = -1;
        }
    }

private:
    bridge::Channel m_channel;
    bool m_allowStill = false;
    std::vector<std::string> m_wrapper;
    std::string m_log;
    pid_t m_pid = -1;
    bool m_closed = false;
};

QString writeScene(const QString &dir, const QString &clearColour)
{
    testutil::writeFile(QDir(dir).filePath(u"scene.json"_s),
                        QByteArray("{\"general\":{\"clearcolor\":\"") + clearColour.toUtf8()
                            + "\",\"orthogonalprojection\":{\"width\":1920,\"height\":1080}},\"objects\":[]}");
    return dir;
}

// A scene of one layer that shows a video texture (the clip of videofixtures.h) through a plain shader of its own.
QString writeVideoScene(const QString &dir)
{
    const auto file = [&](const QString &name, const QByteArray &bytes) { testutil::writeFile(QDir(dir).filePath(name), bytes); };
    std::vector<uint8_t> tex;
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            tex.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    const auto tag = [&](const char *t) {
        tex.insert(tex.end(), t, t + 8);
        tex.push_back(0);
    };
    tag("TEXV0005");
    tag("TEXI0001");
    u32(0); u32(32 | 2); u32(64); u32(48); u32(64); u32(48); u32(0);
    tag("TEXB0003");
    u32(1); u32(0xFFFFFFFFu); u32(1); u32(64); u32(48); u32(0);
    u32(sizeof(videofix::kClip)); u32(sizeof(videofix::kClip));
    tex.insert(tex.end(), std::begin(videofix::kClip), std::end(videofix::kClip));
    QDir().mkpath(QDir(dir).filePath(u"materials"_s));
    QDir().mkpath(QDir(dir).filePath(u"models"_s));
    QDir().mkpath(QDir(dir).filePath(u"shaders"_s));
    file(u"materials/clip.tex"_s, QByteArray(reinterpret_cast<const char *>(tex.data()), static_cast<qsizetype>(tex.size())));
    file(u"shaders/vid.vert"_s,
         "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
         "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n");
    file(u"shaders/vid.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
    file(u"materials/vid.json"_s, R"({"passes":[{"shader":"vid","blending":"translucent","textures":["clip"]}]})");
    file(u"models/vid.json"_s, R"({"material":"materials/vid.json"})");
    file(u"scene.json"_s,
         R"({"general":{"clearcolor":"0 0 0","orthogonalprojection":{"width":1920,"height":1080}},"objects":[)"
         R"({"id":1,"name":"v","image":"models/vid.json","origin":"960 540 0","size":"1920 1080"}]})");
    return dir;
}

// Whether the process has a library of that name mapped (libav is loaded by a scene that has a video and by no other).
bool hasLibraryMapped(pid_t pid, const QString &name)
{
    QFile maps(u"/proc/%1/maps"_s.arg(pid));
    return maps.open(QIODevice::ReadOnly) && QString::fromLatin1(maps.readAll()).contains(name);
}

// The text of the engine's log, after the process has exited.
QString readLog(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

class TstSceneVkEngine : public QObject
{
    Q_OBJECT

    bool m_haveVulkan = false;
    bool m_haveDmabuf = false;
    QTemporaryDir m_cache; // XDG_CACHE_HOME of the engines: their shader cache is not the user's

    // Reads the handshake of an engine started with shared memory and returns the ShmBuffers message.
    static bool handshakeShm(Engine &engine, msg::Viewport &viewport, msg::ShmBuffers &buffers, std::optional<bridge::ShmRing> &ring)
    {
        auto hello = engine.next(120000);
        msg::Hello h{};
        if (!hello || !decodeAs(*hello, h) || h.version != bridge::kShortcutVersion) {
            qWarning() << "handshake: no Hello" << (hello ? "of the right kind" : "in time");
            return false;
        }
        auto vp = engine.next(60000);
        if (!vp || !decodeAs(*vp, viewport)) {
            qWarning() << "handshake: no Viewport" << (vp ? "of the right kind" : "in time");
            return false;
        }
        auto shm = engine.next(60000);
        if (!shm || !decodeAs(*shm, buffers) || shm->fds.size() != 1 || !msg::validate(buffers, 1)) {
            qWarning() << "handshake: no ShmBuffers" << (shm ? "of the right kind" : "in time");
            return false;
        }
        ring = bridge::ShmRing::open(std::move(shm->fds[0]), {buffers.count, buffers.width, buffers.height, buffers.stride});
        if (!ring) {
            qWarning() << "handshake: the ring would not open";
        }
        return ring.has_value();
    }

private slots:
    void renderScaleIsParsedAndChecked()
    {
        scene::Options o;
        std::string error;
        const char *good[] = {"murasaki-scene-vk", "--render-scale", "0.75", "--bg", "/x"};
        QVERIFY2(scene::parseOptions(5, good, o, error), error.c_str());
        QCOMPARE(o.renderScale, 0.75f);
        for (const char *bad : {"0", "0.1", "9", "fast"}) {
            const char *argv[] = {"murasaki-scene-vk", "--render-scale", bad};
            scene::Options other;
            QVERIFY2(!scene::parseOptions(3, argv, other, error), bad);
        }
    }

    void inputSwitchesAreParsed()
    {
        const char *argv[] = {"murasaki-scene-vk", "--disable-mouse", "--disable-parallax", "--bg", "/x"};
        scene::Options o;
        std::string error;
        QVERIFY2(scene::parseOptions(5, argv, o, error), error.c_str());
        QVERIFY(o.disableMouse);
        QVERIFY(o.disableParallax);
        scene::Options plain;
        const char *none[] = {"murasaki-scene-vk", "--bg", "/x"};
        QVERIFY(scene::parseOptions(3, none, plain, error));
        QVERIFY(!plain.disableMouse && !plain.disableParallax);
    }

    void initTestCase()
    {
        qputenv("XDG_CACHE_HOME", m_cache.path().toUtf8());
        std::string error;
        auto ctx = scene::VkContext::create({}, error);
        m_haveVulkan = ctx != nullptr;
        m_haveDmabuf = ctx && ctx->dmabufExport();
    }

    void versionAndUsage()
    {
        // Does not need a GPU.
        QCOMPARE(QProcess::execute(QString::fromUtf8(MURASAKI_SCENE_VK_PATH), {u"--version"_s}), 0);
        QCOMPARE(QProcess::execute(QString::fromUtf8(MURASAKI_SCENE_VK_PATH), {u"--murasaki-size"_s, u"bogus"_s}), 2);
        QCOMPARE(QProcess::execute(QString::fromUtf8(MURASAKI_SCENE_VK_PATH), {}), 2); // no bridge fd
    }

    void sharedMemoryHandshakeFramesAndQuit()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Engine engine;
        QVERIFY(engine.start(writeScene(dir.path(), u"0.2 0.4 1.0"_s), u"320x180"_s, true));

        msg::Viewport viewport{};
        msg::ShmBuffers buffers{};
        std::optional<bridge::ShmRing> ring;
        QVERIFY(handshakeShm(engine, viewport, buffers, ring));
        QCOMPARE(viewport.width, 320u);
        QCOMPARE(viewport.height, 180u);
        // a scene without a video never loads libav (its 125 libraries were in every scene's startup and 23 MB of its memory)
        QVERIFY(!hasLibraryMapped(engine.pid(), u"libavcodec"_s));
        QVERIFY(!hasLibraryMapped(engine.pid(), u"libavformat"_s));
        QCOMPARE(std::string(bridge::getString(viewport.name)), std::string("test"));
        QCOMPARE(buffers.count, bridge::kMaxBuffers);
        QCOMPARE(buffers.stride, 320u * 4);
        QVERIFY(buffers.generation >= 1);

        uint64_t lastSeq = 0;
        for (int i = 0; i < 4; ++i) {
            auto frame = engine.next(30000);
            msg::Frame f{};
            QVERIFY(frame && decodeAs(*frame, f));
            QCOMPARE(f.generation, buffers.generation);
            QVERIFY(f.buffer < buffers.count);
            QVERIFY(f.seq > lastSeq);
            lastSeq = f.seq;
            const auto pixels = std::as_const(*ring).frame(f.buffer);
            QVERIFY(pixels.size() == size_t{320} * 180 * 4);
            for (size_t px : {size_t{0}, size_t{319}, size_t{320 * 90 + 50}, size_t{320 * 180 - 1}}) {
                QCOMPARE(int(pixels[px * 4 + 0]), 51);
                QCOMPARE(int(pixels[px * 4 + 1]), 102);
                QCOMPARE(int(pixels[px * 4 + 2]), 255);
                QCOMPARE(int(pixels[px * 4 + 3]), 255);
            }
            engine.send(msg::Release{0, f.buffer, f.generation});
        }
        QVERIFY(engine.mediaWants().has_value() && !*engine.mediaWants()); // a scene with no media slot and no media hook says so

        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    // The engine behind murasaki-sandbox, with the policy murasaki gives it: it still draws, over the same bridge.
    void sandboxedEngineStillRenders()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        if (murasaki::sandbox::kernelAbi() < 1)
            QSKIP("no Landlock on this kernel");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sceneDir = writeScene(dir.path(), u"0.2 0.4 1.0"_s);
        Engine engine;
        engine.wrapWith({MURASAKI_SANDBOX_PATH, "--profile", "scene", "--item", sceneDir.toStdString(), "--require", "--"});
        QVERIFY(engine.start(sceneDir, u"320x180"_s, true));

        msg::Viewport viewport{};
        msg::ShmBuffers buffers{};
        std::optional<bridge::ShmRing> ring;
        QVERIFY(handshakeShm(engine, viewport, buffers, ring));
        // The engine process is the one murasaki-sandbox exec'd, so it carries the helper's no_new_privs.
        QFile status(u"/proc/%1/status"_s.arg(engine.pid()));
        QVERIFY(status.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromLatin1(status.readAll()).contains(QRegularExpression(u"NoNewPrivs:\\s*1"_s)));
        for (int i = 0; i < 3; ++i) {
            auto frame = engine.next(30000);
            msg::Frame f{};
            QVERIFY(frame && decodeAs(*frame, f));
            const auto pixels = std::as_const(*ring).frame(f.buffer);
            QCOMPARE(int(pixels[0]), 51);
            QCOMPARE(int(pixels[1]), 102);
            QCOMPARE(int(pixels[2]), 255);
            engine.send(msg::Release{0, f.buffer, f.generation});
        }
        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    // Video under the real sandbox: the VA driver and the render node must be readable there, or the GPU decoding falls back to the CPU
    // without anyone noticing. The sandboxed engine has to decode the way the open one does.
    void aSandboxedEngineDecodesVideoTheWayAnOpenOneDoes()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        if (murasaki::sandbox::kernelAbi() < 1)
            QSKIP("no Landlock on this kernel");
        const auto decodedOn = [&](bool sandboxed) {
            QTemporaryDir dir;
            QTemporaryDir logs;
            if (!dir.isValid() || !logs.isValid())
                return QString();
            const QString sceneDir = writeVideoScene(dir.path());
            const QString logPath = logs.filePath(u"engine.log"_s);
            Engine engine;
            if (sandboxed)
                engine.wrapWith({MURASAKI_SANDBOX_PATH, "--profile", "scene", "--item", sceneDir.toStdString(), "--require", "--"});
            engine.logTo(logPath.toStdString());
            if (!engine.start(sceneDir, u"160x90"_s, true))
                return QString();
            msg::Viewport viewport{};
            msg::ShmBuffers buffers{};
            std::optional<bridge::ShmRing> ring;
            if (!handshakeShm(engine, viewport, buffers, ring))
                return QString();
            auto frame = engine.next(30000);
            msg::Frame f{};
            if (!frame || !decodeAs(*frame, f))
                return QString();
            // (a sandboxed process is not dumpable: its maps are not for us to read)
            if (!sandboxed && !hasLibraryMapped(engine.pid(), u"libavcodec"_s))
                return QString(); // a scene with a video loads the decoder
            // the clip's first picture is red: the sandbox did not take the picture away
            const auto pixels = std::as_const(*ring).frame(f.buffer);
            const bool red = pixels[0] > 200 && pixels[1] < 60 && pixels[2] < 60;
            engine.send(msg::Release{0, f.buffer, f.generation});
            engine.send(msg::Quit{});
            engine.wait(30000);
            const QString log = readLog(logPath);
            const QRegularExpression way(u"decoded on the (GPU \\(VAAPI\\)|CPU)"_s);
            const auto match = way.match(log);
            return red && match.hasMatch() ? match.captured(1) : QString();
        };
        const QString open = decodedOn(false);
        QVERIFY2(!open.isEmpty(), "the open engine plays the clip");
        const QString sandboxed = decodedOn(true);
        QVERIFY2(!sandboxed.isEmpty(), "the sandboxed engine plays the clip");
        QCOMPARE(sandboxed, open);
        qInfo() << "video decoded on the" << open << "(open) and the" << sandboxed << "(sandboxed)";
    }

    // A missing plugin is no crash: --check refuses the scene with a clear reason, and with the plugin it accepts.
    void aMissingVideoPluginIsAReasonNotACrash()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sceneDir = writeVideoScene(dir.path());
        const auto check = [&](const QString &plugin) {
            QProcess process;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            if (!plugin.isEmpty())
                env.insert(u"MURASAKI_SCENE_VK_VIDEO_PLUGIN"_s, plugin);
            process.setProcessEnvironment(env);
            process.start(QString::fromLatin1(MURASAKI_SCENE_VK_PATH), {u"--check"_s, sceneDir});
            if (!process.waitForFinished(120000))
                return QStringList{u"timeout"_s};
            return QStringList{QString::number(process.exitCode()), QString::fromUtf8(process.readAllStandardOutput())};
        };
        const QStringList without = check(u"none"_s);
        QCOMPARE(without.value(0), u"0"_s); // a verdict is not a failure
        QVERIFY2(without.value(1).contains(u"\"ok\":false"_s), qPrintable(without.join(u" ")));
        QVERIFY2(without.value(1).contains(u"video plugin"_s), qPrintable(without.join(u" ")));
        const QStringList with = check({});
        QCOMPARE(with.value(0), u"0"_s);
        QVERIFY2(with.value(1).contains(u"\"ok\":true"_s) || with.value(1).contains(u"\"ok\": true"_s), qPrintable(with.join(u" ")));
    }

    void withoutReleasesTheEngineStopsAtThreeFrames()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        Engine engine;
        QVERIFY(engine.start(writeScene(dir.path(), u"0 0 0"_s), u"64x64"_s, true));
        msg::Viewport viewport{};
        msg::ShmBuffers buffers{};
        std::optional<bridge::ShmRing> ring;
        QVERIFY(handshakeShm(engine, viewport, buffers, ring));
        int frames = 0;
        while (auto m = engine.next(400)) {
            msg::Frame f{};
            if (decodeAs(*m, f))
                ++frames;
        }
        QCOMPARE(frames, 3); // every buffer is held: the engine skips frames instead of overwriting one
        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    void pauseStopsFramesAndResumeRestartsThem()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        Engine engine;
        QVERIFY(engine.start(writeScene(dir.path(), u"0.5 0.5 0.5"_s), u"64x64"_s, true));
        msg::Viewport viewport{};
        msg::ShmBuffers buffers{};
        std::optional<bridge::ShmRing> ring;
        QVERIFY(handshakeShm(engine, viewport, buffers, ring));

        // Releases every frame that arrives during `ms`; returns how many there were.
        const auto releaseFor = [&](int ms) {
            int frames = 0;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) {
                auto m = engine.next(20);
                msg::Frame f{};
                if (m && decodeAs(*m, f)) {
                    ++frames;
                    engine.send(msg::Release{0, f.buffer, f.generation});
                }
            }
            return frames;
        };
        // (on a loaded machine the frames come late: wait for them, not for a fixed time)
        const auto framesFlow = [&](int wanted) {
            int got = 0;
            QElapsedTimer waited;
            waited.start();
            while (got < wanted && waited.elapsed() < 30000)
                got += releaseFor(100);
            return got >= wanted;
        };
        QVERIFY(framesFlow(4));
        engine.send(msg::Pause{1});
        releaseFor(150); // frames that were already in flight
        QCOMPARE(releaseFor(300), 0);
        engine.send(msg::Pause{0});
        QVERIFY(framesFlow(1));
        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    void aStillSceneSendsOnlyKeepAliveFramesAndAnswersChanges()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        Engine engine;
        engine.allowStill();
        QVERIFY(engine.start(writeScene(dir.path(), u"0.5 0.5 0.5"_s), u"64x64"_s, true));
        msg::Viewport viewport{};
        msg::ShmBuffers buffers{};
        std::optional<bridge::ShmRing> ring;
        QVERIFY(handshakeShm(engine, viewport, buffers, ring));
        const auto releaseFor = [&](int ms) {
            int frames = 0;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) {
                auto m = engine.next(20);
                msg::Frame f{};
                if (m && decodeAs(*m, f)) {
                    ++frames;
                    engine.send(msg::Release{0, f.buffer, f.generation});
                }
            }
            return frames;
        };
        int first = 0;
        for (QElapsedTimer waited; first < 1 && (waited.isValid() || (waited.start(), true)) && waited.elapsed() < 30000;)
            first += releaseFor(100); // the first frame (late on a loaded machine)
        QVERIFY(first >= 1);
        // at 60 fps a drawing engine would send about 130 frames in 2.2 s; a still one only its keep-alive
        const int still = releaseFor(2200);
        QVERIFY2(still >= 1 && still <= 4, qPrintable(QString::number(still)));
        // a change of mode is answered at once
        engine.send(msg::Scaling{static_cast<uint32_t>(msg::ScalingMode::Fit)});
        int answered = 0;
        for (QElapsedTimer waited; answered < 1 && (waited.isValid() || (waited.start(), true)) && waited.elapsed() < 30000;)
            answered += releaseFor(100);
        QVERIFY(answered >= 1);
        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    void spanViewIsAcceptedAndABadOneEndsTheEngine()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        {
            Engine engine;
            QVERIFY(engine.start(writeScene(dir.path(), u"0.2 0.4 1.0"_s), u"64x36"_s, true));
            msg::Viewport viewport{};
            msg::ShmBuffers buffers{};
            std::optional<bridge::ShmRing> ring;
            QVERIFY(handshakeShm(engine, viewport, buffers, ring));
            engine.send(msg::makeSpanView(true, 128, 36, 64, 0, 64, 36));
            // Frames keep coming, and the flat clear colour is the same wherever the viewport lies.
            int frames = 0;
            while (frames < 6) {
                auto m = engine.next(30000);
                msg::Frame f{};
                QVERIFY(m && decodeAs(*m, f));
                ++frames;
                if (frames >= 4) {
                    const auto pixels = std::as_const(*ring).frame(f.buffer);
                    QCOMPARE(int(pixels[(64 * 18 + 10) * 4 + 2]), 255);
                }
                engine.send(msg::Release{0, f.buffer, f.generation});
            }
            engine.send(msg::makeSpanView(false, 0, 0, 0, 0, 0, 0));
            engine.send(msg::Quit{});
            QCOMPARE(engine.wait(30000), 0);
        }
        {
            Engine engine;
            QVERIFY(engine.start(writeScene(dir.path(), u"0 0 0"_s), u"64x36"_s, true));
            msg::Viewport viewport{};
            msg::ShmBuffers buffers{};
            std::optional<bridge::ShmRing> ring;
            QVERIFY(handshakeShm(engine, viewport, buffers, ring));
            msg::SpanView bad = msg::makeSpanView(true, 128, 36, 64, 0, 64, 36);
            bad.x = 1000 * bridge::kSpanUnit; // far outside the canvas
            engine.send(bad);
            int code = engine.wait(30000);
            QVERIFY2(code > 0, qPrintable(QString::number(code)));
        }
    }

    void resizeAnnouncesANewGeneration()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        Engine engine;
        QVERIFY(engine.start(writeScene(dir.path(), u"1 0 0"_s), u"128x72"_s, true));
        msg::Viewport viewport{};
        msg::ShmBuffers first{};
        std::optional<bridge::ShmRing> ring;
        QVERIFY(handshakeShm(engine, viewport, first, ring));

        engine.send(msg::Resize{0, 64, 36});
        std::optional<msg::ShmBuffers> second;
        std::optional<bridge::ShmRing> ring2;
        for (int i = 0; i < 50 && !second; ++i) {
            auto m = engine.next(1000);
            QVERIFY(m.has_value());
            msg::Frame f{};
            if (decodeAs(*m, f)) {
                engine.send(msg::Release{0, f.buffer, f.generation});
                continue;
            }
            msg::ShmBuffers b{};
            if (decodeAs(*m, b)) {
                second = b;
                QCOMPARE(m->fds.size(), size_t{1});
                ring2 = bridge::ShmRing::open(std::move(m->fds[0]), {b.count, b.width, b.height, b.stride});
            }
        }
        QVERIFY(second.has_value());
        QCOMPARE(second->width, 64u);
        QCOMPARE(second->height, 36u);
        QVERIFY(second->generation > first.generation);
        QVERIFY(ring2.has_value());
        // Frames of the new generation show the new buffers.
        bool sawNew = false;
        for (int i = 0; i < 30 && !sawNew; ++i) {
            auto m = engine.next(1000);
            QVERIFY(m.has_value());
            msg::Frame f{};
            if (decodeAs(*m, f) && f.generation == second->generation) {
                const auto pixels = std::as_const(*ring2).frame(f.buffer);
                QCOMPARE(pixels.size(), size_t{64} * 36 * 4);
                QCOMPARE(int(pixels[0]), 255);
                QCOMPARE(int(pixels[1]), 0);
                sawNew = true;
            }
            if (decodeAs(*m, f))
                engine.send(msg::Release{0, f.buffer, f.generation});
        }
        QVERIFY(sawNew);
        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    void dmabufHandshakeAndTransportRequests()
    {
        if (!m_haveDmabuf)
            QSKIP("no Vulkan device with dma-buf export");
        QTemporaryDir dir;
        Engine engine;
        QVERIFY(engine.start(writeScene(dir.path(), u"0 1 0"_s), u"96x54"_s, false));
        QVERIFY(engine.next(120000).has_value()); // Hello
        QVERIFY(engine.next(30000).has_value());  // Viewport
        auto first = engine.next(30000);
        msg::DmabufBuffers d{};
        QVERIFY(first && decodeAs(*first, d));
        QVERIFY(msg::validate(d, first->fds.size()));
        QCOMPARE(d.count, bridge::kMaxBuffers);
        QCOMPARE(d.planes, 1u);
        QCOMPARE(d.fourcc, 0x34324241u);
        QCOMPARE(d.width, 96u);
        QVERIFY(d.plane[0][0].stride >= 96 * 4);

        // The consumer cannot import the tiled layout: linear next, then shared memory.
        engine.send(msg::RequestLinear{0});
        std::optional<msg::DmabufBuffers> linear;
        for (int i = 0; i < 50 && !linear; ++i) {
            auto m = engine.next(1000);
            QVERIFY(m.has_value());
            msg::DmabufBuffers b{};
            if (decodeAs(*m, b))
                linear = b;
        }
        QVERIFY(linear.has_value());
        QCOMPARE(linear->modifier, uint64_t{0});
        QVERIFY(linear->generation > d.generation);

        engine.send(msg::RequestShm{0});
        std::optional<msg::ShmBuffers> shm;
        std::optional<bridge::ShmRing> ring;
        for (int i = 0; i < 50 && !shm; ++i) {
            auto m = engine.next(1000);
            QVERIFY(m.has_value());
            msg::ShmBuffers b{};
            if (decodeAs(*m, b)) {
                shm = b;
                ring = bridge::ShmRing::open(std::move(m->fds[0]), {b.count, b.width, b.height, b.stride});
            }
        }
        QVERIFY(shm.has_value() && ring.has_value());
        QVERIFY(shm->generation > linear->generation);
        for (int i = 0; i < 30; ++i) {
            auto m = engine.next(1000);
            QVERIFY(m.has_value());
            msg::Frame f{};
            if (!decodeAs(*m, f))
                continue;
            if (f.generation != shm->generation) {
                engine.send(msg::Release{0, f.buffer, f.generation});
                continue;
            }
            const auto pixels = std::as_const(*ring).frame(f.buffer);
            QCOMPARE(int(pixels[0]), 0);
            QCOMPARE(int(pixels[1]), 255);
            QCOMPARE(int(pixels[2]), 0);
            break;
        }
        engine.send(msg::Quit{});
        QCOMPARE(engine.wait(30000), 0);
    }

    void engineExitsWhenTheConsumerGoesAway()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        Engine engine;
        QVERIFY(engine.start(writeScene(dir.path(), u"0 0 0"_s), u"32x32"_s, true));
        QVERIFY(engine.next(120000).has_value());
        engine.channel() = bridge::Channel(); // closes our end
        QCOMPARE(engine.wait(30000), 0);
    }

    void aLostDeviceExitsWithItsOwnStatus()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir;
        qputenv("MURASAKI_SCENE_VK_FAULT", "devicelost"); // test hook: the third frame throws VK_ERROR_DEVICE_LOST
        Engine engine;
        const bool started = engine.start(writeScene(dir.path(), u"0 0 0"_s), u"32x32"_s, true);
        qunsetenv("MURASAKI_SCENE_VK_FAULT");
        QVERIFY(started);
        std::optional<msg::Error> error;
        while (auto m = engine.next(10000)) {
            msg::Frame f{};
            msg::Error e{};
            if (decodeAs(*m, f))
                engine.send(msg::Release{0, f.buffer, f.generation});
            else if (decodeAs(*m, e)) {
                error = e;
                break;
            }
        }
        QVERIFY(error.has_value()); // the peer is told before the process goes
        QCOMPARE(error->code, 3u);  // a runtime error, not an init error
        QCOMPARE(engine.wait(30000), 3);
    }

    void missingSceneReportsAnErrorAndFails()
    {
        if (!m_haveVulkan)
            QSKIP("no Vulkan 1.3 device");
        QTemporaryDir dir; // no scene.json
        Engine engine;
        QVERIFY(engine.start(dir.path(), u"32x32"_s, true));
        std::optional<msg::Error> error;
        while (auto m = engine.next(10000)) {
            msg::Error e{};
            if (decodeAs(*m, e)) {
                error = e;
                break;
            }
        }
        QVERIFY(error.has_value());
        QCOMPARE(error->code, 2u);
        QCOMPARE(engine.wait(30000), 1);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkEngine)
#include "tst_scenevk_engine.moc"
