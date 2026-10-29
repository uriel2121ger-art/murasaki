// murasaki-scene-vk core: device setup and the exportable frame buffers. Every test skips itself without a Vulkan
// 1.3 device; the dma-buf tests also need the driver to export and a render node for the EGL side.

#include "core/bridgesink.h"
#include "core/context.h"
#include "core/framebuffers.h"
#include "dmabufimport.h"
#include "eglcontext.h"

#include <QTest>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <memory>

using namespace murasaki;
using namespace murasaki::scene;
using testegl::EglContext;
using testegl::hasExt;
using testegl::haveRenderNode;

namespace {

using GlBindTexture = void (*)(unsigned, unsigned);
using GlGetTexImage = void (*)(unsigned, int, unsigned, unsigned, void *);
constexpr unsigned kTexture2D = 0x0DE1;
constexpr unsigned kRgba = 0x1908;
constexpr unsigned kUnsignedByte = 0x1401;

using Rgba = std::array<uint8_t, 4>;

// The clear colour is chosen so every channel converts to an exact byte (multiples of 1/255).
constexpr std::array<std::array<float, 4>, 3> kColours = {{
    {0.0f, 0.4f, 1.0f, 1.0f},
    {1.0f, 0.2f, 0.0f, 1.0f},
    {0.6f, 0.6f, 0.2f, 1.0f},
}};
constexpr std::array<Rgba, 3> kBytes = {{{0, 102, 255, 255}, {255, 51, 0, 255}, {153, 153, 51, 255}}};

void clearBuffer(VkContext &ctx, FrameBufferSet &set, uint32_t index, const std::array<float, 4> &colour)
{
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        set.cmdAcquire(cb, index, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue value;
        std::memcpy(value.float32, colour.data(), sizeof(value.float32));
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, set.image(index), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        set.cmdRelease(cb, index, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    });
    set.finishFrame(index);
}

} // namespace

class TstSceneVkCore : public QObject
{
    Q_OBJECT

    std::unique_ptr<VkContext> m_ctx;
    QString m_skip;

private slots:
    void initTestCase()
    {
        qputenv("MURASAKI_VK_VALIDATE", "1"); // honoured when the layer is installed
        std::string error;
        m_ctx = VkContext::create(contextOptionsFromEnvironment(), error);
        if (!m_ctx)
            m_skip = QString::fromStdString(error);
    }

    void cleanupTestCase()
    {
        m_ctx.reset();
        QCOMPARE(validationMessageCount(), 0u);
    }

    void contextReportsADevice()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        QVERIFY(!m_ctx->deviceName().empty());
        QVERIFY(m_ctx->device() != VK_NULL_HANDLE);
    }

    // The queue asks for the low global priority where the driver has the extension; MURASAKI_VK_QUEUE_PRIORITY=medium (or the
    // option) keeps the ordinary one.
    void theQueueHasTheLowPriorityWhereTheDriverGivesIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        QVERIFY(contextOptionsFromEnvironment().lowPriority);
        qputenv("MURASAKI_VK_QUEUE_PRIORITY", "medium");
        QVERIFY(!contextOptionsFromEnvironment().lowPriority);
        qunsetenv("MURASAKI_VK_QUEUE_PRIORITY");
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(m_ctx->physicalDevice(), nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(m_ctx->physicalDevice(), nullptr, &count, exts.data());
        const bool offered = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties &e) {
            return std::strcmp(e.extensionName, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) == 0
                || std::strcmp(e.extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME) == 0;
        });
        if (offered)
            QVERIFY2(m_ctx->lowPriority(), "the driver offers global priority: the queue must be low");
        ContextOptions ordinary;
        ordinary.lowPriority = false;
        std::string error;
        auto other = VkContext::create(ordinary, error);
        QVERIFY2(other, error.c_str());
        QVERIFY(!other->lowPriority());
    }

    void badDeviceSelectorFails()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        ContextOptions options;
        options.device = "no-such-gpu";
        std::string error;
        QVERIFY(!VkContext::create(options, error));
        QVERIFY(!error.empty());
    }

    void sharedMemoryFramesKeepTheirColour()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        std::string error;
        auto set = FrameBufferSet::create(*m_ctx, 256, 256, Transport::Shm, error);
        QVERIFY2(set, error.c_str());
        QVERIFY(!set->isDmabuf());
        for (uint32_t i = 0; i < kBufferCount; ++i)
            clearBuffer(*m_ctx, *set, i, kColours[i]);
        for (uint32_t i = 0; i < kBufferCount; ++i) {
            const auto frame = set->ring().frame(i);
            QCOMPARE(frame.size(), size_t{256} * 256 * 4);
            for (size_t px : {size_t{0}, size_t{255}, size_t{256 * 128 + 7}, size_t{256 * 256 - 1}}) {
                Rgba got;
                std::memcpy(got.data(), frame.data() + px * 4, 4);
                QCOMPARE(got, kBytes[i]);
            }
        }
    }

    void linearDmabufMapsWithTheClearColour()
    {
        if (!m_ctx || !m_ctx->dmabufExport())
            QSKIP("no Vulkan device with dma-buf export");
        std::string error;
        auto set = FrameBufferSet::create(*m_ctx, 256, 256, Transport::DmabufLinear, error);
        if (!set)
            QSKIP(qPrintable(QString::fromStdString(error)));
        QCOMPARE(set->modifier(), uint64_t{0});
        for (uint32_t i = 0; i < 2; ++i)
            clearBuffer(*m_ctx, *set, i, kColours[i]);
        const auto fds = set->dmabufFds();
        for (uint32_t i = 0; i < 2; ++i) {
            const auto plane = set->plane(i);
            QVERIFY(plane.stride >= 256 * 4);
            const size_t length = plane.offset + size_t{plane.stride} * 256;
            void *map = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fds[i], 0);
            QVERIFY2(map != MAP_FAILED, "cannot mmap the linear dma-buf");
            dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            ::ioctl(fds[i], DMA_BUF_IOCTL_SYNC, &sync);
            const auto *bytes = static_cast<const uint8_t *>(map) + plane.offset;
            for (size_t y : {size_t{0}, size_t{100}, size_t{255}}) {
                for (size_t x : {size_t{0}, size_t{77}, size_t{255}}) {
                    Rgba got;
                    std::memcpy(got.data(), bytes + y * plane.stride + x * 4, 4);
                    QCOMPARE(got, kBytes[i]);
                }
            }
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
            ::ioctl(fds[i], DMA_BUF_IOCTL_SYNC, &sync);
            ::munmap(map, length);
        }
    }

    void exportedDmabufImportsIntoEgl()
    {
        if (!m_ctx || !m_ctx->dmabufExport())
            QSKIP("no Vulkan device with dma-buf export");
        if (!haveRenderNode())
            QSKIP("no /dev/dri/renderD* render node");
        EglContext egl;
        if (!egl.create())
            QSKIP("cannot create a surfaceless desktop-GL EGL context");
        DmabufImporter importer;
        std::string error;
        if (!importer.init(&error))
            QSKIP(qPrintable(QString::fromStdString(error)));
        auto getTexImage = reinterpret_cast<GlGetTexImage>(DmabufImporter::procAddress("glGetTexImage"));
        auto bindTexture = reinterpret_cast<GlBindTexture>(DmabufImporter::procAddress("glBindTexture"));
        QVERIFY(getTexImage && bindTexture);

        auto set = FrameBufferSet::create(*m_ctx, 256, 256, Transport::Dmabuf, error);
        QVERIFY2(set, error.c_str());
        QVERIFY(set->isDmabuf());
        for (uint32_t i = 0; i < 2; ++i)
            clearBuffer(*m_ctx, *set, i, kColours[i]);

        const auto fds = set->dmabufFds();
        for (uint32_t i = 0; i < 2; ++i) {
            DmabufDescription d;
            d.width = 256;
            d.height = 256;
            d.fourcc = set->fourcc();
            d.modifier = set->modifier();
            d.planeCount = 1;
            d.planes[0] = {fds[i], set->plane(i).offset, set->plane(i).stride};
            auto imported = importer.import(d, &error);
            QVERIFY2(imported.has_value(), error.c_str());
            std::vector<uint8_t> pixels(256 * 256 * 4, 0xAA);
            bindTexture(kTexture2D, imported->texture);
            getTexImage(kTexture2D, 0, kRgba, kUnsignedByte, pixels.data());
            bindTexture(kTexture2D, 0);
            importer.destroy(*imported);
            for (size_t px : {size_t{0}, size_t{255}, size_t{256 * 100 + 33}, size_t{256 * 256 - 1}}) {
                Rgba got;
                std::memcpy(got.data(), pixels.data() + px * 4, 4);
                QCOMPARE(got, kBytes[i]);
            }
        }
    }

    void foreignBufferSizeIsRejected()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        std::string error;
        QVERIFY(!FrameBufferSet::create(*m_ctx, 0, 10, Transport::Shm, error));
        QVERIFY(!error.empty());
        QVERIFY(!FrameBufferSet::create(*m_ctx, 100000, 10, Transport::Shm, error));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkCore)
#include "tst_scenevk_core.moc"
