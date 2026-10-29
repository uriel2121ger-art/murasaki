// murasaki-scene-vk view mapping: where the scene lands in an output and the size of the scene target that covers what
// is visible. Pure arithmetic, runs everywhere.

#include "render/viewmapping.h"

#include <QTest>

#include <cmath>

using namespace murasaki::scene;
namespace msg = murasaki::bridge::msg;

namespace {

TargetPlanInput input(msg::ScalingMode mode, float sceneW, float sceneH, uint32_t w, uint32_t h, float renderScale = 1.0f)
{
    TargetPlanInput in;
    in.mode = mode;
    in.scene = {sceneW, sceneH};
    in.outputWidth = w;
    in.outputHeight = h;
    in.renderScale = renderScale;
    in.marginFraction = 0.0f;
    return in;
}

} // namespace

class TstSceneVkViewMapping : public QObject
{
    Q_OBJECT
private slots:
    void bigSceneIsRenderedAtOutputSize()
    {
        // the "5k universe": 5120x2880 on 2560x1440 used to be drawn at 5120x2880
        const TargetPlan plan = planSceneTarget(input(msg::ScalingMode::Fill, 5120, 2880, 2560, 1440));
        QCOMPARE(plan.extent.width, 2560u);
        QCOMPARE(plan.extent.height, 1440u);
        QCOMPARE(plan.windowMin.x, 0.0f);
        QCOMPARE(plan.windowSize.x, 5120.0f);
        QCOMPARE(plan.windowSize.y, 2880.0f);
        QCOMPARE(plan.presentSize.x, 2560.0f);
        QCOMPARE(plan.presentOffset.x, 0.0f);
    }
    void renderScaleShrinksTheTarget()
    {
        const TargetPlan half = planSceneTarget(input(msg::ScalingMode::Fill, 5120, 2880, 2560, 1440, 0.5f));
        QCOMPARE(half.extent.width, 1280u);
        QCOMPARE(half.extent.height, 720u);
        // the window is still the whole scene: only the resolution changed
        QCOMPARE(half.windowSize.x, 5120.0f);
    }
    void smallSceneIsNeverUpscaled()
    {
        const TargetPlan plan = planSceneTarget(input(msg::ScalingMode::Fill, 1280, 720, 2560, 1440));
        QCOMPARE(plan.extent.width, 1280u);
        QCOMPARE(plan.extent.height, 720u);
        QCOMPARE(plan.presentSize.x, 2560.0f);
    }
    void fillCropsToTheVisiblePart()
    {
        // a portrait scene on a landscape output: only the middle band is visible
        const TargetPlan plan = planSceneTarget(input(msg::ScalingMode::Fill, 1080, 1920, 1920, 1080));
        QCOMPARE(plan.presentSize.x, 1920.0f);
        QCOMPARE(plan.presentSize.y, 1080.0f);
        QCOMPARE(plan.extent.width, 1080u);
        QVERIFY(plan.extent.height < 700u); // about 607 scene lines are visible
        QVERIFY(std::fabs(plan.windowSize.y - 607.5f) < 1.5f);
        // centred: the visible band is symmetric about the scene's middle
        QVERIFY(std::fabs(plan.windowMin.y + plan.windowSize.y / 2.0f - 960.0f) < 1.0f);
    }
    void croppedEdgesLandOnWholeOutputPixels()
    {
        TargetPlanInput in = input(msg::ScalingMode::Fill, 1000, 1000, 1000, 301);
        in.marginFraction = 0.013f;
        const TargetPlan plan = planSceneTarget(in);
        const float top = plan.presentOffset.y;
        QVERIFY(std::fabs(top - std::round(top)) < 1e-3f);
        const float bottom = plan.presentOffset.y + plan.presentSize.y;
        QVERIFY(std::fabs(bottom - std::round(bottom)) < 1e-3f);
        // and the visible rows [0, 301) are inside
        QVERIFY(top <= 0.0f);
        QVERIFY(bottom >= 301.0f);
    }
    void marginNeverLeavesTheScene()
    {
        TargetPlanInput in = input(msg::ScalingMode::Fill, 1920, 1080, 1920, 1080);
        in.marginFraction = 0.1f;
        in.extraMargin = 0.05f;
        const TargetPlan plan = planSceneTarget(in);
        QCOMPARE(plan.windowMin.x, 0.0f);
        QCOMPARE(plan.windowMin.y, 0.0f);
        QCOMPARE(plan.windowSize.x, 1920.0f);
        QCOMPARE(plan.windowSize.y, 1080.0f);
    }
    void marginGrowsACroppedWindow()
    {
        TargetPlanInput in = input(msg::ScalingMode::Fill, 1000, 2000, 1000, 500);
        const TargetPlan tight = planSceneTarget(in);
        in.marginFraction = 0.1f;
        const TargetPlan padded = planSceneTarget(in);
        QVERIFY(padded.windowSize.y > tight.windowSize.y);
        QVERIFY(padded.extent.height > tight.extent.height);
    }
    void fitUsesTheWholeScene()
    {
        const TargetPlan plan = planSceneTarget(input(msg::ScalingMode::Fit, 1920, 1080, 1000, 1000));
        QCOMPARE(plan.windowSize.x, 1920.0f);
        QCOMPARE(plan.windowSize.y, 1080.0f);
        QCOMPARE(plan.extent.width, 1000u);
        QVERIFY(plan.extent.height == 562u || plan.extent.height == 563u);
        QVERIFY(plan.presentOffset.y > 0.0f);
    }
    void stretchHasItsOwnDensityPerAxis()
    {
        const TargetPlan plan = planSceneTarget(input(msg::ScalingMode::Stretch, 2000, 1000, 1000, 1000));
        QCOMPARE(plan.extent.width, 1000u);
        QCOMPARE(plan.extent.height, 1000u);
    }
    void spannedOutputShowsItsRectangleOfTheCanvas()
    {
        // a 2000x500 scene over a 2000x500 canvas shown on two 1000x500 outputs: each output draws half of the scene
        TargetPlanInput in = input(msg::ScalingMode::Fill, 2000, 500, 1000, 500);
        in.span = msg::makeSpanView(true, 2000, 500, 1000, 0, 1000, 500);
        const TargetPlan right = planSceneTarget(in);
        QCOMPARE(right.windowSize.x, 1000.0f);
        QCOMPARE(right.windowMin.x, 1000.0f);
        QCOMPARE(right.extent.width, 1000u);
        in.span = msg::makeSpanView(true, 2000, 500, 0, 0, 1000, 500);
        const TargetPlan left = planSceneTarget(in);
        QCOMPARE(left.windowMin.x, 0.0f);
        QCOMPARE(left.windowSize.x, 1000.0f);
    }
    void dimensionIsCapped()
    {
        TargetPlanInput in = input(msg::ScalingMode::Fill, 20000, 10000, 16000, 8000, 2.0f);
        in.maxDimension = 4096;
        const TargetPlan plan = planSceneTarget(in);
        QVERIFY(plan.extent.width <= 4096u);
        QVERIFY(plan.extent.height <= 4096u);
    }
    void windowMapsBackToTheOutput()
    {
        // for every case: window size in scene units / scene units per output pixel == present size in output pixels
        const msg::ScalingMode modes[] = {msg::ScalingMode::Fill, msg::ScalingMode::Fit, msg::ScalingMode::Stretch};
        for (const msg::ScalingMode mode : modes) {
            TargetPlanInput in = input(mode, 3000, 1700, 1366, 768);
            in.marginFraction = 0.03f;
            const TargetPlan plan = planSceneTarget(in);
            const ScaledView view = viewOf(mode, in.scene, in.outputWidth, in.outputHeight, in.span);
            QVERIFY(std::fabs(plan.windowSize.x / in.scene.x * view.sizeX - plan.presentSize.x) < 1e-2f);
            QVERIFY(std::fabs(plan.windowSize.y / in.scene.y * view.sizeY - plan.presentSize.y) < 1e-2f);
        }
    }
};

QTEST_GUILESS_MAIN(TstSceneVkViewMapping)
#include "tst_scenevk_viewmapping.moc"
