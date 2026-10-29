// The Vulkan renderer of the puppet engine (src/puppet/render.*): synthetic models drawn on whatever Vulkan 1.3 device there is (a software one is enough) and read back. Skips without a device.
// Expected pixels are exact where the maths is (flat colours at 1:1), within a couple of levels where blending rounds. No golden checksums of whole pictures are kept: different drivers
// rasterise edges differently, so pixels are probed away from edges.
#include "core/bridgesink.h"
#include "core/context.h"
#include "puppet/render.h"
#include "puppetfake.h"
#include "puppetlimits.h"

#include <QBuffer>
#include <QImage>
#include <QTest>

#include <array>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;
using namespace puppetfake;
namespace scene = murasaki::scene;

namespace {

using Px = std::array<int, 4>;

struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    Px at(uint32_t x, uint32_t y) const
    {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
    uint64_t checksum() const
    {
        uint64_t h = 1469598103934665603ull;
        for (const uint8_t b : rgba)
            h = (h ^ b) * 1099511628211ull;
        return h;
    }
};

bool near(const Px &a, const Px &b, int tol = 2)
{
    for (size_t i = 0; i < 4; ++i)
        if (std::abs(a[i] - b[i]) > tol)
            return false;
    return true;
}

QString show(const Px &p)
{
    return u"(%1,%2,%3,%4)"_s.arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]);
}

#define PX(actual, expected, tol)                                                                                       \
    do {                                                                                                                \
        const Px a_ = (actual);                                                                                         \
        const Px e_ = (expected);                                                                                       \
        QVERIFY2(near(a_, e_, tol), qPrintable(u"got %1, expected %2"_s.arg(show(a_), show(e_))));                      \
    } while (false)

TextureImage solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    TextureImage t;
    t.width = t.height = 1;
    t.rgba = {r, g, b, a};
    premultiplyInPlace(t.rgba);
    return t;
}

// left half opaque white, right half transparent
TextureImage halves()
{
    TextureImage t;
    t.width = 2;
    t.height = 1;
    t.rgba = {255, 255, 255, 255, 0, 0, 0, 0};
    return t;
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

} // namespace

class TestPuppetRender : public QObject
{
    Q_OBJECT
public:
    // a model with `drawables` quad shapes whose frame the test fills
    std::unique_ptr<FakeModel> model(int drawables = 3, Bounds extent = {0, 0, 64, 64})
    {
        auto m = std::make_unique<FakeModel>();
        for (int i = 0; i < drawables; ++i)
            m->shapes.push_back(FakeModel::quadShape());
        m->extent = extent;
        return m;
    }

    Picture draw(PuppetRenderer &r, const Frame &frame, const ViewTransform &view, uint32_t w = 64, uint32_t h = 64)
    {
        scene::GpuImage target = m_ctx->createImage(VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        scene::GpuBuffer readback = m_ctx->createBuffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        m_ctx->submitAndWait([&](VkCommandBuffer cb) {
            barrier(cb, target.handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            r.cmdDraw(cb, target.view(), {w, h}, frame, view);
            barrier(cb, target.handle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cb, target.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle(), 1, &copy);
        });
        readback.invalidate();
        Picture p;
        p.width = w;
        p.height = h;
        p.rgba.assign(static_cast<uint8_t *>(readback.mapped()), static_cast<uint8_t *>(readback.mapped()) + static_cast<size_t>(w) * h * 4);
        return p;
    }

    std::unique_ptr<PuppetRenderer> renderer(const ModelBackend &m, const std::vector<TextureImage> &textures, bool point = true)
    {
        std::string error;
        auto r = PuppetRenderer::create(*m_ctx, m, textures, point, error);
        if (!r)
            qFatal("renderer: %s", error.c_str());
        return r;
    }

    static ViewTransform identity64() { return fitBounds({0, 0, 64, 64}, 64, 64); }

private slots:
    void initTestCase()
    {
        std::string error;
        m_ctx = scene::VkContext::create(scene::contextOptionsFromEnvironment(), error);
        if (!m_ctx)
            QSKIP(qPrintable(QString::fromStdString("no Vulkan 1.3 device: " + error)));
    }
    void cleanupTestCase() { m_ctx.reset(); }

    void aFlatQuadTintedAndFaded();
    void screenTintAndTextureAlpha();
    void blendModes_data();
    void blendModes();
    void masks();
    void maskThresholdAndDodge();
    void groups();
    void viewTransformPlacesTheModel();
    void badItemsAreSkipped();
    void aMaskSourceSharedByManyPartsIsDrawnForEach();
    void aFrameBeyondThePositionBudgetDrawsNothing();
    void positionsBeyondAFloatAreClamped();
    void aRendererThatCannotBeMadeSaysWhy();
    void deterministicAndSensitive();
    void drawsManyFramesWithOneRenderer();
    void textureDecoding();

private:
    std::unique_ptr<scene::VkContext> m_ctx;
};

void TestPuppetRender::aFlatQuadTintedAndFaded()
{
    auto m = model();
    auto r = renderer(*m, {solid(255, 255, 255, 255)});
    Frame f;
    DrawItem item = FakeModel::mesh(0, 16, 16, 32, 32);
    item.tint[0] = 1;
    item.tint[1] = 0;
    item.tint[2] = 0;
    f.items.push_back(item);
    Picture p = draw(*r, f, identity64());
    PX(p.at(32, 32), (Px{255, 0, 0, 255}), 1);
    PX(p.at(4, 4), (Px{0, 0, 0, 0}), 0);   // transparent black outside
    PX(p.at(60, 60), (Px{0, 0, 0, 0}), 0);
    QCOMPARE(r->stats().meshes, 1u);
    // half opacity: premultiplied, so colour and alpha are both halved
    f.items[0].opacity = 0.5;
    p = draw(*r, f, identity64());
    PX(p.at(32, 32), (Px{128, 0, 0, 128}), 2);
    // opacity 0 draws nothing; above 1 is 1
    f.items[0].opacity = 0;
    p = draw(*r, f, identity64());
    PX(p.at(32, 32), (Px{0, 0, 0, 0}), 0);
    f.items[0].opacity = 7;
    p = draw(*r, f, identity64());
    PX(p.at(32, 32), (Px{255, 0, 0, 255}), 1);
}

void TestPuppetRender::screenTintAndTextureAlpha()
{
    auto m = model();
    // S(c, a) = 1 - (1 - c)(1 - screen a): black opaque with screen blue 0.5 is half blue
    auto r = renderer(*m, {solid(0, 0, 0, 255), halves()});
    Frame f;
    DrawItem item = FakeModel::mesh(0, 0, 0, 64, 64);
    item.screenTint[2] = 0.5;
    f.items.push_back(item);
    Picture p = draw(*r, f, identity64());
    PX(p.at(32, 32), (Px{0, 0, 128, 255}), 2);
    // a texture with a transparent half: nearest sampling, the right half shows nothing
    f.items.clear();
    f.items.push_back(FakeModel::mesh(0, 0, 0, 64, 64, 1));
    p = draw(*r, f, identity64());
    PX(p.at(10, 32), (Px{255, 255, 255, 255}), 1);
    PX(p.at(54, 32), (Px{0, 0, 0, 0}), 0);
}

void TestPuppetRender::blendModes_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<Px>("expected");
    // destination: opaque (128, 128, 128); source: opaque (255, 128, 0) drawn over the left half of it
    const auto row = [](const char *name, BlendMode mode, Px expected) { QTest::newRow(name) << int(mode) << expected; };
    row("normal", BlendMode::Normal, {255, 128, 0, 255});
    row("multiply", BlendMode::Multiply, {128, 64, 0, 255});   // S * D
    row("screen", BlendMode::Screen, {255, 192, 128, 255});    // S + D (1 - S)
    row("lighten", BlendMode::Lighten, {255, 128, 128, 255});  // max
    row("linear dodge", BlendMode::LinearDodge, {255, 192, 128, 255});
    row("add glow", BlendMode::AddGlow, {255, 255, 128, 255}); // S + D, clamped
    row("subtract", BlendMode::Subtract, {0, 0, 0, 255});      // D - (1 - D) S, clamped: (128 - 127*255/255, 128 - 127*128/255, 128 - 127*0)... see below
    row("exclusion", BlendMode::Exclusion, {128, 128, 128, 255}); // S (1 - D) + D (1 - S)
    row("inverse", BlendMode::Inverse, {128, 64, 0, 255});     // S (1 - D) + D (1 - Sa) with Sa = 1: S (1 - D)
    row("destination in", BlendMode::DestinationIn, {128, 128, 128, 255});
    row("clip to lower", BlendMode::ClipToLower, {255, 128, 0, 255}); // S Da
    row("slice from lower", BlendMode::SliceFromLower, {0, 0, 0, 0});
    row("overlay falls back to normal", BlendMode::Overlay, {255, 128, 0, 255});
    row("difference falls back to normal", BlendMode::Difference, {255, 128, 0, 255});
}

void TestPuppetRender::blendModes()
{
    QFETCH(int, mode);
    QFETCH(Px, expected);
    // fix the expectations that need arithmetic
    const BlendMode m = static_cast<BlendMode>(mode);
    const double dst = 128 / 255.0;
    const std::array<double, 3> src = {1.0, 128 / 255.0, 0.0};
    if (m == BlendMode::Subtract) {
        for (int c = 0; c < 3; ++c)
            expected[size_t(c)] = int(std::lround(std::clamp(dst - (1 - dst) * src[size_t(c)], 0.0, 1.0) * 255));
    }
    if (m == BlendMode::Exclusion) {
        for (int c = 0; c < 3; ++c)
            expected[size_t(c)] = int(std::lround(std::clamp(src[size_t(c)] * (1 - dst) + dst * (1 - src[size_t(c)]), 0.0, 1.0) * 255));
    }
    if (m == BlendMode::Inverse) {
        for (int c = 0; c < 3; ++c)
            expected[size_t(c)] = int(std::lround(std::clamp(src[size_t(c)] * (1 - dst), 0.0, 1.0) * 255));
    }
    auto fake = model();
    auto r = renderer(*fake, {solid(128, 128, 128, 255), solid(255, 128, 0, 255)});
    Frame f;
    f.items.push_back(FakeModel::mesh(0, 0, 0, 64, 64, 0));
    DrawItem top = FakeModel::mesh(1, 0, 0, 32, 64, 1);
    top.blend = m;
    f.items.push_back(top);
    const Picture p = draw(*r, f, identity64());
    PX(p.at(16, 32), expected, 3);
    PX(p.at(48, 32), (Px{128, 128, 128, 255}), 1); // the right half is untouched
}

void TestPuppetRender::masks()
{
    auto fake = model();
    auto r = renderer(*fake, {solid(255, 255, 255, 255), halves()});
    // the mask source is the textured quad (left half opaque); the masked item is a red quad over the whole picture
    Frame f;
    DrawItem masked = FakeModel::mesh(0, 0, 0, 64, 64, 0);
    masked.tint[1] = masked.tint[2] = 0;
    masked.masks = {{1, false}};
    f.items.push_back(masked);
    f.maskSources.push_back(FakeModel::mesh(1, 0, 0, 64, 64, 1));
    Picture p = draw(*r, f, identity64());
    PX(p.at(10, 32), (Px{255, 0, 0, 255}), 1);
    PX(p.at(54, 32), (Px{0, 0, 0, 0}), 0);
    QCOMPARE(r->stats().maskDraws, 1u);
    // a mask whose source is missing from the frame hides everything (no footprint)
    f.maskSources.clear();
    p = draw(*r, f, identity64());
    PX(p.at(10, 32), (Px{0, 0, 0, 0}), 0);
    // a mask naming a drawable the model does not have is skipped the same way
    f.items[0].masks = {{99, false}};
    p = draw(*r, f, identity64());
    PX(p.at(10, 32), (Px{0, 0, 0, 0}), 0);
    // a later item without masks is not affected by the earlier stencil
    f.items.push_back(FakeModel::mesh(2, 0, 0, 64, 64, 0));
    p = draw(*r, f, identity64());
    PX(p.at(54, 32), (Px{255, 255, 255, 255}), 1);
}

void TestPuppetRender::maskThresholdAndDodge()
{
    auto fake = model();
    auto r = renderer(*fake, {solid(255, 255, 255, 255), halves()});
    Frame f;
    DrawItem masked = FakeModel::mesh(0, 0, 0, 64, 64, 0);
    masked.tint[1] = masked.tint[2] = 0;
    f.items.push_back(masked);
    DrawItem source = FakeModel::mesh(1, 0, 0, 64, 64, 1);
    f.maskSources.push_back(source);
    // Dodge only: everything shows except where the source is
    f.items[0].masks = {{1, true}};
    Picture p = draw(*r, f, identity64());
    PX(p.at(10, 32), (Px{0, 0, 0, 0}), 0);
    PX(p.at(54, 32), (Px{255, 0, 0, 255}), 1);
    // Mask then Dodge of the same source: the later binding wins where both mark
    f.items[0].masks = {{1, false}, {1, true}};
    p = draw(*r, f, identity64());
    PX(p.at(10, 32), (Px{0, 0, 0, 0}), 0);
    PX(p.at(54, 32), (Px{0, 0, 0, 0}), 0); // nothing was marked 1 outside the source
    // the threshold is the source's: a source texel of alpha 0.4 is above 0.3 and below 0.5
    auto half = renderer(*fake, {solid(255, 255, 255, 255), solid(255, 255, 255, 102)});
    f.items[0].masks = {{1, false}};
    f.maskSources[0].maskThreshold = 0.5;
    f.maskSources[0].texture = 1;
    p = draw(*half, f, identity64());
    PX(p.at(32, 32), (Px{0, 0, 0, 0}), 0);
    f.maskSources[0].maskThreshold = 0.3;
    p = draw(*half, f, identity64());
    PX(p.at(32, 32), (Px{255, 0, 0, 255}), 1);
    // a mask on a different item does not leak: two masked items in a row use their own masks
    f.items.push_back(FakeModel::mesh(2, 0, 0, 64, 64, 0));
    f.items.back().tint[0] = f.items.back().tint[2] = 0;
    f.items.back().masks = {{1, true}};
    f.maskSources[0].maskThreshold = 0.3;
    p = draw(*half, f, identity64());
    PX(p.at(32, 32), (Px{255, 0, 0, 255}), 1); // the green one is dodged by the same source and shows nowhere under it; the red shows
}

void TestPuppetRender::groups()
{
    auto fake = model();
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    // two opaque quads inside a group, the group faded to half: the overlap shows the top one faded once, not the two blended
    Frame f;
    DrawItem begin;
    begin.kind = DrawKind::BeginGroup;
    begin.drawable = 0;
    f.items.push_back(begin);
    DrawItem a = FakeModel::mesh(1, 0, 0, 40, 40, 0);
    a.tint[1] = a.tint[2] = 0; // red
    f.items.push_back(a);
    DrawItem b = FakeModel::mesh(2, 24, 24, 40, 40, 0);
    b.tint[0] = b.tint[1] = 0; // blue
    f.items.push_back(b);
    DrawItem end;
    end.kind = DrawKind::EndGroup;
    end.drawable = 0;
    end.opacity = 0.5;
    f.items.push_back(end);
    Picture p = draw(*r, f, identity64());
    PX(p.at(10, 10), (Px{128, 0, 0, 128}), 2);
    PX(p.at(32, 32), (Px{0, 0, 128, 128}), 2); // the overlap: blue over red inside the group, then half
    PX(p.at(56, 56), (Px{0, 0, 128, 128}), 2);
    PX(p.at(60, 4), (Px{0, 0, 0, 0}), 0);
    QCOMPARE(r->stats().groups, 1u);
    // without the group the two quads at half opacity each blend: the overlap is purple-ish, not blue
    Frame g;
    a.opacity = b.opacity = 0.5;
    g.items = {a, b};
    p = draw(*r, g, identity64());
    QVERIFY(p.at(32, 32)[0] > 20); // red is still there under the blue
    // a group blended with Multiply over an opaque backdrop, then a second group: both work in one frame
    Frame h;
    DrawItem back = FakeModel::mesh(1, 0, 0, 64, 64, 0);
    back.tint[0] = back.tint[1] = back.tint[2] = 0.5;
    h.items.push_back(back);
    for (int i = 0; i < 2; ++i) {
        DrawItem bg;
        bg.kind = DrawKind::BeginGroup;
        bg.drawable = 0;
        h.items.push_back(bg);
        DrawItem inner = FakeModel::mesh(2, i * 32.0, 0, 32, 64, 0);
        inner.tint[0] = 1;
        inner.tint[1] = 0.5;
        inner.tint[2] = 0;
        h.items.push_back(inner);
        DrawItem eg;
        eg.kind = DrawKind::EndGroup;
        eg.drawable = 0;
        eg.blend = BlendMode::Multiply;
        h.items.push_back(eg);
    }
    p = draw(*r, h, identity64());
    PX(p.at(16, 32), (Px{128, 64, 0, 255}), 3);   // (1, .5, 0) x (.5, .5, .5)
    PX(p.at(48, 32), (Px{128, 64, 0, 255}), 3);   // the second group, drawn after the first group's image was reused
    QCOMPARE(r->stats().groups, 2u);
    // a masked group: only inside the mask
    Frame k;
    DrawItem kb;
    kb.kind = DrawKind::BeginGroup;
    kb.drawable = 0;
    k.items.push_back(kb);
    DrawItem fill = FakeModel::mesh(2, 0, 0, 64, 64, 0);
    k.items.push_back(fill);
    DrawItem ke;
    ke.kind = DrawKind::EndGroup;
    ke.drawable = 0;
    ke.masks = {{1, false}};
    k.items.push_back(ke);
    auto rm = renderer(*fake, {solid(255, 255, 255, 255), halves()});
    k.items[1].texture = 0;
    k.maskSources.push_back(FakeModel::mesh(1, 0, 0, 64, 64, 1));
    p = draw(*rm, k, identity64());
    PX(p.at(10, 32), (Px{255, 255, 255, 255}), 1);
    PX(p.at(54, 32), (Px{0, 0, 0, 0}), 0);
    // a group that is never closed, and an end without a begin, are handled
    Frame bad;
    bad.items.push_back(begin);
    bad.items.push_back(a);
    p = draw(*r, bad, identity64());
    Frame stray;
    stray.items.push_back(end);
    p = draw(*r, stray, identity64());
    PX(p.at(32, 32), (Px{0, 0, 0, 0}), 0);
}

void TestPuppetRender::viewTransformPlacesTheModel()
{
    // a 100 x 50 model drawn into a 200 x 200 picture: scale 2, centred vertically
    auto fake = model(1, {0, 0, 100, 50});
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    Frame f;
    f.items.push_back(FakeModel::mesh(0, 0, 0, 100, 50, 0));
    const Picture p = draw(*r, f, fitBounds({0, 0, 100, 50}, 200, 200), 200, 200);
    PX(p.at(100, 100), (Px{255, 255, 255, 255}), 1);
    PX(p.at(5, 60), (Px{255, 255, 255, 255}), 1);   // the left edge
    PX(p.at(100, 30), (Px{0, 0, 0, 0}), 0);         // above the model: it spans rows 50 to 150
    PX(p.at(100, 170), (Px{0, 0, 0, 0}), 0);
    // a target of another size is drawn at once (the images of the renderer follow it)
    const Picture q = draw(*r, f, fitBounds({0, 0, 100, 50}, 100, 100), 100, 100);
    PX(q.at(50, 50), (Px{255, 255, 255, 255}), 1);
    PX(q.at(50, 10), (Px{0, 0, 0, 0}), 0);
}

void TestPuppetRender::badItemsAreSkipped()
{
    auto fake = model(2);
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    Frame f;
    f.items.push_back(FakeModel::mesh(7, 0, 0, 10, 10));                 // no such drawable
    DrawItem wrong = FakeModel::mesh(0, 0, 0, 10, 10);
    wrong.positions.pop_back();                                          // not one position per vertex
    f.items.push_back(wrong);
    DrawItem noTexture = FakeModel::mesh(1, 20, 20, 20, 20, -1);        // draws nothing: a blank texture is transparent
    f.items.push_back(noTexture);
    DrawItem badTexture = FakeModel::mesh(1, 20, 20, 20, 20, 9);        // same
    f.items.push_back(badTexture);
    DrawItem negative = FakeModel::mesh(0, 40, 40, 10, 10);
    negative.drawable = -1;
    f.items.push_back(negative);
    const Picture p = draw(*r, f, identity64());
    QCOMPARE(r->stats().skipped, 3u);
    PX(p.at(30, 30), (Px{0, 0, 0, 0}), 0);
    // many more meshes than the buffer first had room for: it grows, and all of them are drawn
    Frame many;
    for (int i = 0; i < 100; ++i)
        many.items.push_back(FakeModel::mesh(0, 0, 0, 64, 64));
    draw(*r, many, identity64());
    QCOMPARE(r->stats().skipped, 0u);
    QCOMPARE(r->stats().meshes, 100u);
    // items of any kind with absurd numbers do not crash
    Frame odd;
    DrawItem nan = FakeModel::mesh(0, 0, 0, 64, 64);
    nan.positions[0].x = NAN;
    nan.opacity = NAN;
    nan.tint[0] = INFINITY;
    odd.items.push_back(nan);
    draw(*r, odd, identity64());
}

void TestPuppetRender::aMaskSourceSharedByManyPartsIsDrawnForEach()
{
    // a round mask of 5000 vertices (a fan) used by 10 parts: each use writes the source's positions again, so the buffer, first sized for the meshes drawn once, has to grow
    constexpr int kRim = 4999;
    auto fake = std::make_unique<FakeModel>();
    Shape disc;
    disc.uvs.assign(kRim + 1, {0.5, 0.5});
    for (int i = 1; i < kRim; ++i)
        disc.indices.insert(disc.indices.end(), {0, uint16_t(i), uint16_t(i + 1)});
    fake->shapes.push_back(disc);
    for (int k = 0; k < 10; ++k)
        fake->shapes.push_back(FakeModel::quadShape());
    fake->extent = {0, 0, 64, 64};
    DrawItem source;
    source.kind = DrawKind::Mesh;
    source.drawable = 0;
    source.texture = 0;
    source.positions.push_back({32, 32});
    for (int i = 0; i < kRim; ++i) {
        const double a = 6.283185307179586 * i / kRim;
        source.positions.push_back({32 + 28 * std::cos(a), 32 + 28 * std::sin(a)});
    }
    Frame f;
    f.maskSources.push_back(source);
    for (int k = 0; k < 10; ++k) {
        DrawItem part = FakeModel::mesh(k + 1, 14 + (k % 5) * 8, 22 + (k / 5) * 14, 4, 4, 0);
        part.masks = {{0, false}};
        f.items.push_back(part);
    }
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    const Picture p = draw(*r, f, identity64());
    QCOMPARE(r->stats().skipped, 0u);
    QCOMPARE(r->stats().maskDraws, 10u);
    QCOMPARE(r->stats().meshes, 10u);
    for (int k = 0; k < 10; ++k)
        PX(p.at(16 + (k % 5) * 8, 24 + (k / 5) * 14), (Px{255, 255, 255, 255}), 1); // every part shows, not only the first two
    // a second frame with the same shape: nothing is lost
    draw(*r, f, identity64());
    QCOMPARE(r->stats().skipped, 0u);
}

void TestPuppetRender::aFrameBeyondThePositionBudgetDrawsNothing()
{
    // 129 meshes of 65535 vertices: more positions than kMaxPositionVertices, so the frame is cleared and every item counts as skipped
    auto fake = std::make_unique<FakeModel>();
    Shape big;
    big.uvs.assign(65535, {0, 0});
    big.indices = {0, 1, 2};
    fake->shapes.push_back(big);
    fake->extent = {0, 0, 64, 64};
    DrawItem item;
    item.kind = DrawKind::Mesh;
    item.drawable = 0;
    item.texture = 0;
    item.positions.assign(65535, {0, 0});
    item.positions[1] = {64, 0};
    item.positions[2] = {64, 64};
    Frame f;
    for (int i = 0; i < 129; ++i)
        f.items.push_back(item);
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    const Picture p = draw(*r, f, identity64());
    QCOMPARE(r->stats().meshes, 0u);
    QCOMPARE(r->stats().skipped, 129u);
    PX(p.at(50, 20), (Px{0, 0, 0, 0}), 0);
    // and a frame that fits is drawn afterwards by the same renderer
    Frame ok;
    ok.items.push_back(item);
    const Picture q = draw(*r, ok, identity64());
    QCOMPARE(r->stats().meshes, 1u);
    PX(q.at(50, 20), (Px{255, 255, 255, 255}), 1);
}

void TestPuppetRender::positionsBeyondAFloatAreClamped()
{
    // doubles that no float holds (1e300, infinities, NaN) from a backend that did not clamp them: converted without trouble, the frame is drawn
    auto fake = model(1);
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    Frame f;
    DrawItem wide = FakeModel::mesh(0, 0, 0, 64, 64, 0);
    wide.positions[1].x = 1e300;
    wide.positions[2] = {1e300, -1e300};
    wide.positions[3].y = INFINITY;
    f.items.push_back(wide);
    DrawItem nan = FakeModel::mesh(0, 0, 0, 64, 64, 0);
    nan.positions[0].x = NAN;
    f.items.push_back(nan);
    draw(*r, f, identity64());
    QCOMPARE(r->stats().meshes, 2u);
    QCOMPARE(r->stats().skipped, 0u);
}

void TestPuppetRender::aRendererThatCannotBeMadeSaysWhy()
{
    // a texture with no pixels is the model's: the engine ends with "model refused", not "no GPU"
    auto fake = model(1);
    std::string error;
    CreateFailure failure = CreateFailure::None;
    TextureImage empty;
    auto none = PuppetRenderer::create(*m_ctx, *fake, {empty}, true, error, &failure);
    QVERIFY(!none);
    QCOMPARE(failure, CreateFailure::Model);
    QVERIFY(!error.empty());
    // and a good one says nothing went wrong
    auto good = PuppetRenderer::create(*m_ctx, *fake, {solid(255, 255, 255, 255)}, true, error, &failure);
    QVERIFY(good);
    QCOMPARE(failure, CreateFailure::None);
}

void TestPuppetRender::deterministicAndSensitive()
{
    auto fake = model();
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    Frame f;
    DrawItem a = FakeModel::mesh(0, 8, 8, 30, 30);
    a.tint[1] = 0.5;
    f.items.push_back(a);
    const uint64_t first = draw(*r, f, identity64()).checksum();
    QCOMPARE(draw(*r, f, identity64()).checksum(), first); // the same frame, the same picture
    auto other = renderer(*fake, {solid(255, 255, 255, 255)});
    QCOMPARE(draw(*other, f, identity64()).checksum(), first); // another renderer too
    f.items[0].positions = FakeModel::rect(12, 8, 30, 30);
    QVERIFY(draw(*r, f, identity64()).checksum() != first);     // a scripted change of a parameter shows
}

void TestPuppetRender::drawsManyFramesWithOneRenderer()
{
    auto fake = model();
    auto r = renderer(*fake, {solid(255, 255, 255, 255)});
    uint64_t last = 0;
    for (int i = 0; i < 200; ++i) {
        Frame f;
        DrawItem a = FakeModel::mesh(0, i % 40, 8, 20, 20);
        f.items.push_back(a);
        const Picture p = draw(*r, f, identity64());
        PX(p.at(i % 40 + 10, 18), (Px{255, 255, 255, 255}), 1);
        last = p.checksum();
    }
    QVERIFY(last != 0);
}

void TestPuppetRender::textureDecoding()
{
    // PNG through Qt, TGA by hand; both come out premultiplied
    QImage image(2, 1, QImage::Format_ARGB32);
    image.setPixel(0, 0, qRgba(200, 100, 50, 128));
    image.setPixel(1, 0, qRgba(10, 20, 30, 255));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(image.save(&buffer, "PNG"));
    TextureImage t;
    std::string error;
    QVERIFY2(decodeTexture(reinterpret_cast<const uint8_t *>(png.constData()), size_t(png.size()), t, error), error.c_str());
    QCOMPARE(t.width, 2u);
    QCOMPARE(t.height, 1u);
    QVERIFY(std::abs(int(t.rgba[0]) - 100) <= 1);  // 200 * 128 / 255
    QVERIFY(std::abs(int(t.rgba[1]) - 50) <= 1);
    QCOMPARE(int(t.rgba[3]), 128);
    QCOMPARE(int(t.rgba[4]), 10);                  // opaque: unchanged
    // an uncompressed 32-bit TGA of 1 x 1 (BGRA)
    std::vector<uint8_t> tga = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 32, 8, 50, 100, 200, 255};
    TextureImage u;
    QVERIFY2(decodeTexture(tga.data(), tga.size(), u, error), error.c_str());
    QCOMPARE(int(u.rgba[0]), 200);
    QCOMPARE(int(u.rgba[2]), 50);
    // garbage and an oversize picture are refused, with a reason
    const uint8_t junk[] = {1, 2, 3, 4, 5};
    TextureImage v;
    QVERIFY(!decodeTexture(junk, sizeof junk, v, error));
    QVERIFY(!error.empty());
    QVERIFY(!decodeTexture(nullptr, 0, v, error));
    std::vector<uint8_t> huge = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x40, 1, 0, 32, 8}; // width 0x4001 = 16385 > kMaxTextureSide
    QVERIFY(!decodeTexture(huge.data(), huge.size(), v, error));
    QCOMPARE(int(kMaxTextureSide), 8192);
    // the renderer refuses a texture with no pixels
    auto fake = model();
    std::string e;
    TextureImage empty;
    QVERIFY(!PuppetRenderer::create(*m_ctx, *fake, {empty}, true, e));
    QVERIFY(!e.empty());
    // premultiplying leaves opaque and transparent pixels alone and rounds the rest
    std::vector<uint8_t> px = {255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 128};
    premultiplyInPlace(px);
    QCOMPARE(int(px[0]), 255);
    QCOMPARE(int(px[4]), 0);
    QCOMPARE(int(px[8]), 128);
}

QTEST_GUILESS_MAIN(TestPuppetRender)
#include "tst_puppetrender.moc"
