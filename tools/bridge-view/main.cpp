// bridge-view: a headless consumer of the Murasaki bridge.
//
//   bridge-view --out-dir DIR [options] -- PROGRAM [ARGS...]
//
// Launches PROGRAM with `--murasaki-bridge-fd N --murasaki-size WxH` appended (the same way BridgeItem launches a
// host or the scene engine), reads the frames it publishes through the shared BridgeClient + dmabufimport pieces
// on a surfaceless EGL context of its own, and writes some of them to DIR as PNGs.
//
// Timeline: wait for the first frame, let the content settle, then observe for a while (frames are counted,
// fingerprinted and a few saved), then the optional checks run one after the other: Resize, Pause.
//
// Options:
//   --out-dir DIR           where the PNGs and summary.txt go (required)
//   --size WxH              size passed to the producer (default 640x360)
//   --frames N              PNGs to save during the observation (default 3)
//   --settle-ms MS          wait after the first frame before observing (default 1500)
//   --observe-ms MS         observation window (default 4000)
//   --startup-timeout S     give up when no frame arrives (default 60)
//   --expect-nonblack       fail unless a frame has real picture content
//   --expect-animated       fail unless the observed frames differ from each other
//   --fail-fast             stop at once (before the other checks) when no frame arrives in the first part of the
//                           observation: static or stuck content, so a caller can try another item
//   --check-pause           send Pause: no frames may arrive for 2 s, and they resume after the unpause
//   --check-resize WxH      send Resize: frames of that size must arrive
//   --pointer X,Y[,BUTTONS] send a Pointer message after the first frame (bit 0 left, bit 1 right, bit 2 middle)
//   --producer-log FILE     send the producer's stdout and stderr to FILE (default: forward them)
//   --prefix NAME           PNG file name prefix (default "frame")
//
// Exit status: 0 when every requested check passed, 1 when one failed, 2 on a usage or setup error.

#include "bridge/channel.h"
#include "bridgeclient.h"
#include "dmabufimport.h"
#include "eglcontext.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTimer>

#include <sys/prctl.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <set>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace murasaki::bridge;

namespace {

using GlGenFramebuffers = void (*)(int, unsigned *);
using GlDeleteFramebuffers = void (*)(int, const unsigned *);
using GlBindFramebuffer = void (*)(unsigned, unsigned);
using GlFramebufferTexture2D = void (*)(unsigned, unsigned, unsigned, unsigned, int);
using GlCheckFramebufferStatus = unsigned (*)(unsigned);
using GlReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void *);
using GlPixelStorei = void (*)(unsigned, int);

constexpr unsigned kFramebuffer = 0x8D40;
constexpr unsigned kColorAttachment0 = 0x8CE0;
constexpr unsigned kTexture2D = 0x0DE1;
constexpr unsigned kFramebufferComplete = 0x8CD5;
constexpr unsigned kRgba = 0x1908;
constexpr unsigned kUnsignedByte = 0x1401;
constexpr unsigned kPackAlignment = 0x0D05;

template <class F>
F proc(const char *name)
{
    return reinterpret_cast<F>(DmabufImporter::procAddress(name));
}

// What a frame looks like, cheaply: is there picture, and a fingerprint of a sparse sample of it.
struct FrameStats {
    double meanLuma = 0;     // 0..255
    double brightShare = 0;  // share of samples with any channel above 24
    uint64_t fingerprint = 0;
};

FrameStats statsOf(const QImage &image)
{
    FrameStats stats;
    const int stepX = std::max(1, image.width() / 160);
    const int stepY = std::max(1, image.height() / 90);
    uint64_t hash = 1469598103934665603ull;
    uint64_t samples = 0, bright = 0;
    double luma = 0;
    for (int y = 0; y < image.height(); y += stepY) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); x += stepX) {
            const uchar *p = line + x * 4;
            luma += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            bright += (p[0] > 24 || p[1] > 24 || p[2] > 24) ? 1 : 0;
            for (int c = 0; c < 3; ++c)
                hash = (hash ^ p[c]) * 1099511628211ull;
            ++samples;
        }
    }
    if (samples) {
        stats.meanLuma = luma / double(samples);
        stats.brightShare = double(bright) / double(samples);
    }
    stats.fingerprint = hash;
    return stats;
}

// Turns bridge frames into QImages. Dma-bufs are imported once per buffer set and read back through an FBO.
class FrameReader
{
public:
    ~FrameReader() { dropSet(); }

    bool init()
    {
        if (!testegl::haveRenderNode() || !m_egl.create()) {
            m_error = "no OpenGL context on a render node";
            return false;
        }
        std::string error;
        if (!m_importer.init(&error))
            m_dmabufError = QString::fromStdString(error);
        m_genFb = proc<GlGenFramebuffers>("glGenFramebuffers");
        m_delFb = proc<GlDeleteFramebuffers>("glDeleteFramebuffers");
        m_bindFb = proc<GlBindFramebuffer>("glBindFramebuffer");
        m_fbTex = proc<GlFramebufferTexture2D>("glFramebufferTexture2D");
        m_fbStatus = proc<GlCheckFramebufferStatus>("glCheckFramebufferStatus");
        m_readPixels = proc<GlReadPixels>("glReadPixels");
        m_pixelStore = proc<GlPixelStorei>("glPixelStorei");
        if (!m_genFb || !m_bindFb || !m_fbTex || !m_readPixels) {
            m_error = "missing OpenGL entry points";
            return false;
        }
        return true;
    }
    const QString &error() const { return m_error; }

    // A null image: the frame could not be read (the failure was reported to the producer where that helps).
    QImage read(const BridgeFrame &frame, BridgeFrameQueue &queue)
    {
        const BridgeBuffers &set = *frame.buffers;
        if (frame.buffers != m_set) {
            dropSet();
            m_set = frame.buffers;
        }
        QImage image(int(set.width), int(set.height), QImage::Format_RGBA8888);
        if (image.isNull())
            return {};
        if (set.kind == BridgeBuffers::Kind::Shm) {
            const auto pixels = set.ring->frame(frame.buffer);
            const uint32_t stride = set.ring->geometry().stride;
            if (pixels.empty())
                return {};
            for (int y = 0; y < image.height(); ++y)
                std::memcpy(image.scanLine(y), pixels.data() + size_t(y) * stride, size_t(image.width()) * 4);
        } else if (!readDmabuf(frame, queue, image)) {
            return {};
        }
        if (frame.flipY)
            image = image.flipped(Qt::Vertical);
        // The producer's alpha is whatever Qt Quick left; the picture is what matters.
        return image.convertToFormat(QImage::Format_RGBX8888);
    }

private:
    bool readDmabuf(const BridgeFrame &frame, BridgeFrameQueue &queue, QImage &image)
    {
        const BridgeBuffers &set = *frame.buffers;
        if (!m_importer.ready()) {
            queue.reportImportFailure(frame.generation());
            return false;
        }
        auto &slot = m_imported[frame.buffer];
        if (!slot) {
            DmabufDescription d;
            d.width = set.width;
            d.height = set.height;
            d.fourcc = set.fourcc;
            d.modifier = set.modifier;
            d.planeCount = set.planes;
            for (uint32_t p = 0; p < set.planes; ++p)
                d.planes[p] = {set.dmabufFd(frame.buffer, p), set.offset[frame.buffer][p], set.stride[frame.buffer][p]};
            std::string error;
            slot = m_importer.import(d, &error);
            if (!slot) {
                std::fprintf(stderr, "bridge-view: dma-buf import failed: %s\n", error.c_str());
                queue.reportImportFailure(frame.generation());
                return false;
            }
            queue.reportImportSuccess(frame.generation());
        }
        if (!m_fbo)
            m_genFb(1, &m_fbo);
        m_bindFb(kFramebuffer, m_fbo);
        m_fbTex(kFramebuffer, kColorAttachment0, kTexture2D, slot->texture, 0);
        if (m_fbStatus && m_fbStatus(kFramebuffer) != kFramebufferComplete)
            return false;
        if (m_pixelStore)
            m_pixelStore(kPackAlignment, 4);
        m_readPixels(0, 0, image.width(), image.height(), kRgba, kUnsignedByte, image.bits());
        m_bindFb(kFramebuffer, 0);
        return true;
    }

    void dropSet()
    {
        for (auto &slot : m_imported) {
            if (slot)
                m_importer.destroy(*slot);
            slot.reset();
        }
        if (m_fbo && m_delFb)
            m_delFb(1, &m_fbo);
        m_fbo = 0;
        m_set.reset();
    }

    testegl::EglContext m_egl;
    DmabufImporter m_importer;
    QString m_error;
    QString m_dmabufError;
    std::shared_ptr<const BridgeBuffers> m_set;
    std::array<std::optional<ImportedDmabuf>, kMaxBuffers> m_imported;
    unsigned m_fbo = 0;
    GlGenFramebuffers m_genFb = nullptr;
    GlDeleteFramebuffers m_delFb = nullptr;
    GlBindFramebuffer m_bindFb = nullptr;
    GlFramebufferTexture2D m_fbTex = nullptr;
    GlCheckFramebufferStatus m_fbStatus = nullptr;
    GlReadPixels m_readPixels = nullptr;
    GlPixelStorei m_pixelStore = nullptr;
};

std::optional<QSize> parseSize(const QString &text)
{
    const QStringList parts = text.split(u'x');
    bool wOk = false, hOk = false;
    const int w = parts.value(0).toInt(&wOk);
    const int h = parts.value(1).toInt(&hOk);
    if (parts.size() != 2 || !wOk || !hOk || w < 1 || h < 1 || w > 16384 || h > 16384)
        return std::nullopt;
    return QSize(w, h);
}

class Viewer : public QObject
{
public:
    struct Config {
        QString outDir;
        QString prefix = u"frame"_s;
        QString program;
        QStringList arguments;
        QSize size{640, 360};
        int frames = 3;
        int settleMs = 1500;
        int observeMs = 4000;
        int startupTimeoutS = 60;
        bool expectNonblack = false;
        bool expectAnimated = false;
        bool failFast = false;
        bool checkPause = false;
        QString producerLog;
        std::optional<QSize> checkResize;
        std::optional<std::array<int, 3>> pointer;
    };

    explicit Viewer(Config config) : m_config(std::move(config)) {}

    int run()
    {
        if (!m_reader.init()) {
            std::fprintf(stderr, "bridge-view: %s\n", qPrintable(m_reader.error()));
            return 2;
        }
        auto pair = makeSocketPair();
        if (!pair) {
            std::fprintf(stderr, "bridge-view: cannot create the socket pair\n");
            return 2;
        }
        const int childFd = pair->second.get();
        m_process = new QProcess(this);
        if (m_config.producerLog.isEmpty()) {
            m_process->setProcessChannelMode(QProcess::ForwardedChannels);
        } else {
            // The producer's output goes to its own file, so bridge-view's report stays readable.
            m_process->setProcessChannelMode(QProcess::MergedChannels);
            m_process->setStandardOutputFile(m_config.producerLog);
        }
        m_process->setProgram(m_config.program);
        m_process->setArguments(m_config.arguments
                                + QStringList{u"--murasaki-bridge-fd"_s, QString::number(childFd),
                                              u"--murasaki-size"_s,
                                              u"%1x%2"_s.arg(m_config.size.width()).arg(m_config.size.height())});
        const pid_t parentPid = ::getpid();
        m_process->setChildProcessModifier([childFd, parentPid] {
            const int flags = ::fcntl(childFd, F_GETFD);
            if (flags >= 0)
                ::fcntl(childFd, F_SETFD, flags & ~FD_CLOEXEC);
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parentPid)
                ::_exit(1);
        });
        connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (!m_finished)
                finish(false, u"the producer exited (%1%2) before the checks were done"_s.arg(code).arg(
                                  status == QProcess::CrashExit ? u", crashed"_s : QString()));
        });
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false, u"cannot start "_s + m_config.program);
        });

        m_client = new BridgeClient(std::move(pair->first), this);
        m_client->setPeerName(u"producer"_s);
        m_queue = m_client->frames();
        connect(m_client, &BridgeClient::frameAvailable, this, [this] { onFrames(); });
        connect(m_client, &BridgeClient::lost, this, [this](const QString &reason) {
            if (!m_finished)
                finish(false, u"bridge lost: "_s + reason);
        });

        m_process->start();
        pair->second.reset();

        m_clock.start();
        QTimer::singleShot(m_config.startupTimeoutS * 1000, this, [this] {
            if (!m_firstFrameMs)
                finish(false, u"no frame within %1 s"_s.arg(m_config.startupTimeoutS));
        });
        return QCoreApplication::exec();
    }

private:
    enum class Phase { Waiting, Settling, Observing, Resize, PauseSettle, PauseCount, PauseResume, Done };

    void onFrames()
    {
        while (auto frame = m_queue->takeFrame()) {
            QImage image = m_reader.read(*frame, *m_queue);
            m_queue->release(*frame);
            if (image.isNull())
                continue;
            ++m_totalFrames;
            m_lastImage = image;
            m_lastSize = image.size();
            const FrameStats stats = statsOf(image);
            if (!m_firstFrameMs) {
                m_firstFrameMs = m_clock.elapsed();
                std::printf("first frame after %lld ms (%dx%d)\n", qlonglong(*m_firstFrameMs), image.width(), image.height());
                std::fflush(stdout);
                if (m_config.pointer && m_client)
                    m_client->sendPointer((*m_config.pointer)[0], (*m_config.pointer)[1], uint32_t((*m_config.pointer)[2]));
                m_phase = Phase::Settling;
                QTimer::singleShot(m_config.settleMs, this, [this] { beginObserving(); });
            }
            if (m_phase == Phase::Observing) {
                ++m_observedFrames;
                m_fingerprints.insert(stats.fingerprint);
                m_maxLuma = std::max(m_maxLuma, stats.meanLuma);
                m_maxBright = std::max(m_maxBright, stats.brightShare);
            } else if (m_phase == Phase::PauseCount) {
                ++m_framesWhilePaused;
            } else if (m_phase == Phase::PauseResume) {
                if (++m_framesAfterResume >= 2)
                    pauseDone();
            } else if (m_phase == Phase::Resize && image.size() == *m_config.checkResize) {
                m_resizeSeen = true;
                save(u"resized"_s);
                resizeDone();
            }
        }
    }

    void beginObserving()
    {
        m_phase = Phase::Observing;
        const int saves = std::max(0, m_config.frames);
        for (int i = 0; i < saves; ++i) {
            const int at = m_config.observeMs * (2 * i + 1) / (2 * std::max(1, saves));
            QTimer::singleShot(at, this, [this, i] { save(QString::number(i + 1).rightJustified(2, u'0')); });
        }
        if (m_config.failFast) {
            QTimer::singleShot(std::min(3000, m_config.observeMs / 2), this, [this] {
                if (m_phase == Phase::Observing && m_observedFrames == 0)
                    finish(false, u"no frames while observing (static or stuck content)"_s);
            });
        }
        QTimer::singleShot(m_config.observeMs, this, [this] {
            if (m_phase == Phase::Observing)
                observationDone();
        });
    }

    void save(const QString &suffix)
    {
        if (m_lastImage.isNull() || m_config.outDir.isEmpty())
            return;
        const QString path = u"%1/%2-%3.png"_s.arg(m_config.outDir, m_config.prefix, suffix);
        if (m_lastImage.save(path))
            std::printf("saved %s\n", qPrintable(path));
        else
            std::printf("cannot save %s\n", qPrintable(path));
        std::fflush(stdout);
    }

    void observationDone()
    {
        const double seconds = m_config.observeMs / 1000.0;
        std::printf("observed %llu frames in %.1f s (%.1f fps), %zu distinct, brightest mean luma %.1f, "
                    "bright share %.3f\n",
                    (unsigned long long)m_observedFrames, seconds, double(m_observedFrames) / seconds,
                    m_fingerprints.size(), m_maxLuma, m_maxBright);
        std::fflush(stdout);
        if (m_config.expectNonblack)
            check(u"non-black"_s, m_maxBright >= 0.01 && m_maxLuma >= 3.0);
        if (m_config.expectAnimated)
            check(u"animated"_s, m_fingerprints.size() >= 3);
        startNextCheck();
    }

    void startNextCheck()
    {
        if (m_config.checkResize && !m_resizeStarted) {
            m_resizeStarted = true;
            m_phase = Phase::Resize;
            m_client->sendResize(uint32_t(m_config.checkResize->width()), uint32_t(m_config.checkResize->height()));
            QTimer::singleShot(8000, this, [this] {
                if (m_phase == Phase::Resize)
                    resizeDone();
            });
            return;
        }
        if (m_config.checkPause && !m_pauseStarted) {
            m_pauseStarted = true;
            m_phase = Phase::PauseSettle;
            m_client->sendPause(true);
            // Frames already in flight (and the one the pause itself triggers) are allowed to land.
            QTimer::singleShot(1200, this, [this] {
                m_phase = Phase::PauseCount;
                QTimer::singleShot(2000, this, [this] {
                    check(u"pause stops frames"_s, m_framesWhilePaused == 0, u"%1 frames while paused"_s.arg(m_framesWhilePaused));
                    m_phase = Phase::PauseResume;
                    m_client->sendPause(false);
                    QTimer::singleShot(8000, this, [this] {
                        if (m_phase == Phase::PauseResume)
                            pauseDone();
                    });
                });
            });
            return;
        }
        finish(m_failures.isEmpty(), {});
    }

    void resizeDone()
    {
        check(u"resize"_s, m_resizeSeen, u"no %1x%2 frame arrived"_s.arg(m_config.checkResize->width()).arg(m_config.checkResize->height()));
        m_phase = Phase::Done;
        startNextCheck();
    }

    void pauseDone()
    {
        check(u"frames resume after the unpause"_s, m_framesAfterResume >= 2);
        m_phase = Phase::Done;
        startNextCheck();
    }

    void check(const QString &name, bool ok, const QString &detail = {})
    {
        std::printf("CHECK %s: %s%s\n", qPrintable(name), ok ? "PASS" : "FAIL",
                    ok || detail.isEmpty() ? "" : qPrintable(u" ("_s + detail + u")"_s));
        std::fflush(stdout);
        if (!ok)
            m_failures << name;
    }

    void finish(bool ok, const QString &why)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_phase = Phase::Done;
        if (!why.isEmpty()) {
            std::printf("%s: %s\n", ok ? "note" : "FAIL", qPrintable(why));
            m_failures << why;
        }
        std::printf("RESULT %s frames=%llu\n", m_failures.isEmpty() ? "PASS" : "FAIL", (unsigned long long)m_totalFrames);
        std::fflush(stdout);
        m_exit = m_failures.isEmpty() ? 0 : 1;
        // Ask the producer to quit; it also quits when the socket closes. Kill it if it dawdles.
        if (m_client) {
            m_client->sendQuit();
            QTimer::singleShot(100, this, [this] { m_client->close(); });
        }
        QTimer::singleShot(8000, this, [this] {
            if (m_process && m_process->state() != QProcess::NotRunning) {
                m_process->kill();
            }
        });
        if (m_process && m_process->state() != QProcess::NotRunning) {
            connect(m_process, &QProcess::finished, this, [this] { QCoreApplication::exit(m_exit); });
        } else {
            QTimer::singleShot(0, this, [this] { QCoreApplication::exit(m_exit); });
        }
    }

    Config m_config;
    FrameReader m_reader;
    QProcess *m_process = nullptr;
    BridgeClient *m_client = nullptr;
    std::shared_ptr<BridgeFrameQueue> m_queue;
    QElapsedTimer m_clock;
    std::optional<qint64> m_firstFrameMs;
    Phase m_phase = Phase::Waiting;
    QImage m_lastImage;
    QSize m_lastSize;
    uint64_t m_totalFrames = 0, m_observedFrames = 0, m_framesWhilePaused = 0, m_framesAfterResume = 0;
    std::set<uint64_t> m_fingerprints;
    double m_maxLuma = 0, m_maxBright = 0;
    bool m_resizeStarted = false, m_resizeSeen = false, m_pauseStarted = false, m_finished = false;
    QStringList m_failures;
    int m_exit = 0;
};

} // namespace

int main(int argc, char *argv[])
{
    // Everything after "--" belongs to the producer.
    int own = argc;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") {
            own = i;
            break;
        }
    }
    QCoreApplication app(own, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(u"Headless Murasaki bridge consumer: bridge-view --out-dir DIR [options] -- PROGRAM [ARGS...]"_s);
    parser.addHelpOption();
    parser.addOptions({
        {u"out-dir"_s, u"Directory for the PNGs."_s, u"dir"_s},
        {u"size"_s, u"Size passed to the producer."_s, u"WxH"_s, u"640x360"_s},
        {u"frames"_s, u"PNGs to save while observing."_s, u"n"_s, u"3"_s},
        {u"settle-ms"_s, u"Wait after the first frame."_s, u"ms"_s, u"1500"_s},
        {u"observe-ms"_s, u"Observation window."_s, u"ms"_s, u"4000"_s},
        {u"startup-timeout"_s, u"Seconds to wait for the first frame."_s, u"s"_s, u"60"_s},
        {u"expect-nonblack"_s, u"Fail unless a frame has picture content."_s},
        {u"expect-animated"_s, u"Fail unless the observed frames differ."_s},
        {u"fail-fast"_s, u"Stop when no frame arrives early in the observation."_s},
        {u"check-pause"_s, u"Check that Pause stops the frames and the unpause resumes them."_s},
        {u"check-resize"_s, u"Send Resize and expect frames of that size."_s, u"WxH"_s},
        {u"pointer"_s, u"Send a Pointer message after the first frame."_s, u"x,y[,buttons]"_s},
        {u"prefix"_s, u"PNG name prefix."_s, u"name"_s, u"frame"_s},
        {u"producer-log"_s, u"File for the producer's output."_s, u"file"_s},
    });
    parser.process(app);

    Viewer::Config config;
    config.outDir = parser.value(u"out-dir"_s);
    config.prefix = parser.value(u"prefix"_s);
    config.producerLog = parser.value(u"producer-log"_s);
    if (own + 1 >= argc || config.outDir.isEmpty()) {
        std::fprintf(stderr, "usage: bridge-view --out-dir DIR [options] -- PROGRAM [ARGS...]\n");
        return 2;
    }
    config.program = QString::fromLocal8Bit(argv[own + 1]);
    for (int i = own + 2; i < argc; ++i)
        config.arguments << QString::fromLocal8Bit(argv[i]);
    const auto size = parseSize(parser.value(u"size"_s));
    if (!size) {
        std::fprintf(stderr, "bridge-view: bad --size\n");
        return 2;
    }
    config.size = *size;
    config.frames = parser.value(u"frames"_s).toInt();
    config.settleMs = parser.value(u"settle-ms"_s).toInt();
    config.observeMs = std::max(100, parser.value(u"observe-ms"_s).toInt());
    config.startupTimeoutS = std::max(1, parser.value(u"startup-timeout"_s).toInt());
    config.expectNonblack = parser.isSet(u"expect-nonblack"_s);
    config.expectAnimated = parser.isSet(u"expect-animated"_s);
    config.failFast = parser.isSet(u"fail-fast"_s);
    config.checkPause = parser.isSet(u"check-pause"_s);
    if (parser.isSet(u"check-resize"_s)) {
        config.checkResize = parseSize(parser.value(u"check-resize"_s));
        if (!config.checkResize) {
            std::fprintf(stderr, "bridge-view: bad --check-resize\n");
            return 2;
        }
    }
    if (parser.isSet(u"pointer"_s)) {
        const QStringList parts = parser.value(u"pointer"_s).split(u',');
        if (parts.size() < 2 || parts.size() > 3) {
            std::fprintf(stderr, "bridge-view: bad --pointer\n");
            return 2;
        }
        config.pointer = std::array<int, 3>{parts[0].toInt(), parts[1].toInt(), parts.value(2).toInt()};
    }
    QDir().mkpath(config.outDir);

    Viewer viewer(std::move(config));
    return viewer.run();
}
