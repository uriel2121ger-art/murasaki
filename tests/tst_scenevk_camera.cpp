// murasaki-scene-vk camera math: the perspective projection of the canvas and of a window of it, the look-at basis with any up,
// the Euler angles that put a camera object on a basis, and the order camera paths are played in. Pure arithmetic, runs everywhere.

#include "render/camera.h"

#include <QTest>

#include <cmath>
#include <set>

using namespace murasaki::scene;

namespace {

constexpr float kPi = 3.14159265358979f;

Vec4 transform(const Mat4 &m, Vec3 p)
{
    Vec4 r;
    r.x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
    r.y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
    r.z = m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3);
    r.w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
    return r;
}

// normalized device coordinates of a point
Vec3 ndc(const Mat4 &m, Vec3 p)
{
    const Vec4 c = transform(m, p);
    return {c.x / c.w, c.y / c.w, c.z / c.w};
}

bool close(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b));
}

bool close(Vec3 a, Vec3 b, float eps = 1e-4f)
{
    return close(a.x, b.x, eps) && close(a.y, b.y, eps) && close(a.z, b.z, eps);
}

QString show(Vec3 v)
{
    return QStringLiteral("(%1, %2, %3)").arg(v.x).arg(v.y).arg(v.z);
}

#define VERIFY_VEC(actual, expected)                                                                                          \
    do {                                                                                                                      \
        const Vec3 a_ = (actual);                                                                                             \
        const Vec3 e_ = (expected);                                                                                           \
        QVERIFY2(close(a_, e_), qPrintable(QStringLiteral("got %1, expected %2").arg(show(a_), show(e_))));                    \
    } while (false)

const Vec2 kCanvas{3840.0f, 2160.0f};

} // namespace

class TstSceneVkCamera : public QObject
{
    Q_OBJECT
private slots:
    void aPointOnTheLineOfSightLandsInTheMiddle()
    {
        const Mat4 p = perspectiveWindow(50.0f, kCanvas, {0, 0}, kCanvas, 0.1f, 100.0f);
        const Vec3 c = ndc(p, {0.0f, 0.0f, -7.0f});
        QVERIFY(close(c.x, 0.0f) && close(c.y, 0.0f));
        // and through a look-at camera anywhere: its centre lands in the middle of the picture
        const CameraBasis b = lookAtBasis({3, -2, 9}, {-4, 1, 2}, {0, 1, 0});
        const Vec3 m = ndc(p * viewMatrix(b), {-4, 1, 2});
        QVERIFY2(close(m.x, 0.0f) && close(m.y, 0.0f), qPrintable(show(m)));
    }

    void theFieldOfViewIsVerticalAndTheAspectIsTheCanvas()
    {
        const float fov = 50.0f;
        const Mat4 p = perspectiveWindow(fov, kCanvas, {0, 0}, kCanvas, 0.1f, 100.0f);
        const float d = 10.0f;
        const float half = d * std::tan(fov * 0.5f * kPi / 180.0f);
        // the top of the field of view is the top of the picture, at any distance
        QVERIFY(close(ndc(p, {0.0f, half, -d}).y, 1.0f));
        QVERIFY(close(ndc(p, {0.0f, -2.0f * half, -2.0f * d}).y, -1.0f));
        // across, the picture is wider by the canvas's aspect
        QVERIFY(close(ndc(p, {half * 16.0f / 9.0f, 0.0f, -d}).x, 1.0f));
        // a wider fov shows more: the same point comes nearer the middle
        const Mat4 wide = perspectiveWindow(80.0f, kCanvas, {0, 0}, kCanvas, 0.1f, 100.0f);
        QVERIFY(ndc(wide, {0.0f, half, -d}).y < 0.7f);
    }

    void nearAndFarAreTheEndsOfTheDepthRange()
    {
        const Mat4 p = perspectiveWindow(50.0f, kCanvas, {0, 0}, kCanvas, 0.5f, 40.0f);
        QVERIFY(close(ndc(p, {0.0f, 0.0f, -0.5f}).z, -1.0f));
        QVERIFY(close(ndc(p, {0.0f, 0.0f, -40.0f}).z, 1.0f, 1e-3f));
        QVERIFY(std::fabs(ndc(p, {0.0f, 0.0f, -10.0f}).z) < 1.0f);
        // behind the camera w goes negative (clipped)
        QVERIFY(transform(p, {0.0f, 0.0f, 1.0f}).w < 0.0f);
        // nonsense falls back to something drawable
        const Mat4 q = perspectiveWindow(std::nanf(""), {0, 0}, {0, 0}, {0, 0}, -1.0f, -5.0f);
        QVERIFY(std::isfinite(ndc(q, {0.0f, 0.0f, -5.0f}).z));
    }

    void aWindowOfTheCanvasIsCroppedAndAZoomNarrowsTheView()
    {
        const Mat4 whole = perspectiveWindow(50.0f, kCanvas, {0, 0}, kCanvas, 0.1f, 100.0f);
        // the right half of the canvas: the middle of the picture is its left edge
        const Mat4 right = perspectiveWindow(50.0f, kCanvas, {1920.0f, 0.0f}, {1920.0f, 2160.0f}, 0.1f, 100.0f);
        QVERIFY(close(ndc(right, {0.0f, 0.0f, -5.0f}).x, -1.0f));
        const Vec3 corner{3.0f, 2.0f, -10.0f};
        QVERIFY(close(ndc(right, corner).x, 2.0f * ndc(whole, corner).x - 1.0f));
        QVERIFY(close(ndc(right, corner).y, ndc(whole, corner).y));
        // zoom 2 (half the canvas each way about its centre) doubles what is seen, still centred
        const Mat4 zoomed = perspectiveWindow(50.0f, kCanvas, {960.0f, 540.0f}, {1920.0f, 1080.0f}, 0.1f, 100.0f);
        QVERIFY(close(ndc(zoomed, corner).x, 2.0f * ndc(whole, corner).x));
        QVERIFY(close(ndc(zoomed, corner).y, 2.0f * ndc(whole, corner).y));
        QVERIFY(close(ndc(zoomed, {0.0f, 0.0f, -3.0f}).x, 0.0f));
    }

    void aFartherLayerIsSmaller()
    {
        // a unit square at depth 5 and the same at depth 10: half the size on the picture
        const Mat4 p = perspectiveWindow(50.0f, kCanvas, {0, 0}, kCanvas, 0.1f, 100.0f) * viewMatrix(lookAtBasis({0, 0, 0}, {0, 0, -1}, {0, 1, 0}));
        const float nearWidth = ndc(p, {0.5f, 0.0f, -5.0f}).x - ndc(p, {-0.5f, 0.0f, -5.0f}).x;
        const float farWidth = ndc(p, {0.5f, 0.0f, -10.0f}).x - ndc(p, {-0.5f, 0.0f, -10.0f}).x;
        QVERIFY(close(farWidth, nearWidth * 0.5f));
    }

    void lookAtTakesAnyUp()
    {
        // looking along +x with z up
        CameraBasis b = lookAtBasis({1, 2, 3}, {4, 2, 3}, {0, 0, 1});
        VERIFY_VEC(b.forward, (Vec3{1, 0, 0}));
        VERIFY_VEC(b.up, (Vec3{0, 0, 1}));
        VERIFY_VEC(b.right, (Vec3{0, -1, 0}));
        const Mat4 v = viewMatrix(b);
        VERIFY_VEC((Vec3{transform(v, {4, 2, 3}).x, transform(v, {4, 2, 3}).y, transform(v, {4, 2, 3}).z}), (Vec3{0, 0, -3}));
        const Vec4 above = transform(v, {1, 2, 4});
        VERIFY_VEC((Vec3{above.x, above.y, above.z}), (Vec3{0, 1, 0}));
        // an up that leans along the line of sight is straightened
        b = lookAtBasis({0, 0, 0}, {0, 0, -1}, {0, 1, 1});
        VERIFY_VEC(b.up, (Vec3{0, 1, 0}));
        // upside down: up -y turns the picture over
        b = lookAtBasis({0, 0, 0}, {0, 0, -1}, {0, -1, 0});
        VERIFY_VEC(b.right, (Vec3{-1, 0, 0}));
        // an up along the line of sight (or none) falls back to the world's y, or z when looking along y
        b = lookAtBasis({0, 0, 0}, {0, 0, -1}, {0, 0, 1});
        VERIFY_VEC(b.up, (Vec3{0, 1, 0}));
        b = lookAtBasis({0, 0, 0}, {0, -5, 0}, {0, 1, 0});
        VERIFY_VEC(b.forward, (Vec3{0, -1, 0}));
        QVERIFY(close(camvec::length(b.up), 1.0f) && close(camvec::dot(b.up, b.forward), 0.0f));
        // the eye on the centre looks down -z
        b = lookAtBasis({2, 2, 2}, {2, 2, 2}, {0, 1, 0});
        VERIFY_VEC(b.forward, (Vec3{0, 0, -1}));
    }

    void aCameraObjectLooksDownItsOwnNegativeZ()
    {
        // angles 0: the world's -z, y up; scale does not matter
        CameraBasis b = basisFromWorld(translation({1, 2, 3}) * scaling({0.5f, 0.5f, 0.5f}));
        VERIFY_VEC(b.eye, (Vec3{1, 2, 3}));
        VERIFY_VEC(b.forward, (Vec3{0, 0, -1}));
        VERIFY_VEC(b.up, (Vec3{0, 1, 0}));
        // a quarter turn about y looks down -x (3816342243's camera: -pi, pi/2, -pi is the same turn)
        b = basisFromWorld(rotationEuler({-kPi, kPi / 2.0f, -kPi}));
        VERIFY_VEC(b.forward, (Vec3{-1, 0, 0}));
        VERIFY_VEC(b.up, (Vec3{0, 1, 0}));
    }

    void eulerAnglesPutACameraObjectOnABasis()
    {
        const Vec3 targets[][2] = {{{3, -2, 9}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}}, {{1, 0, 0}, {0, 1, 0}}, {{0.2f, 0.9f, -0.1f}, {0, 0, 1}},
                                   {{-1, 0.3f, 0.2f}, {0.3f, 1, 0}}, {{0, 0, 1}, {0, -1, 0}}};
        for (const auto &t : targets) {
            const CameraBasis want = lookAtBasis({5, 6, 7}, camvec::add({5, 6, 7}, t[0]), t[1]);
            const Vec3 angles = eulerFromBasis(want);
            const CameraBasis got = basisFromWorld(translation({5, 6, 7}) * rotationEuler(angles));
            VERIFY_VEC(got.forward, want.forward);
            VERIFY_VEC(got.up, want.up);
        }
    }

    void depthLandsInZeroToOneAfterTheShadersRemap()
    {
        // the translated vertex shaders take OpenGL's depth to Vulkan's: z' = (z + w) / 2 (shaders/translate.cpp)
        const Mat4 p = perspectiveWindow(50.0f, kCanvas, {0, 0}, kCanvas, 0.01f, 10000.0f);
        const auto depth = [&](float distance) {
            const Vec4 c = transform(p, {0.0f, 0.0f, -distance});
            return (c.z + c.w) * 0.5f / c.w;
        };
        QVERIFY(close(depth(0.01f), 0.0f));
        QVERIFY(close(depth(10000.0f), 1.0f, 1e-3f));
        QVERIFY(depth(1.0f) > 0.0f && depth(1.0f) < 1.0f);
        QVERIFY(depth(100.0f) < depth(1000.0f));
    }

    void theFovIsKeptInOneRange()
    {
        QCOMPARE(clampFov(0.0f), kMinFovDegrees);
        QCOMPARE(clampFov(500.0f), kMaxFovDegrees);
        QCOMPARE(clampFov(std::nanf("")), 50.0f);
        QCOMPARE(clampFov(42.5f), 42.5f);
    }

    void farFromTheOriginAndMirroredCamerasKeepTheirAxes()
    {
        // the axes come straight from the matrix: no digits lost 10000 units out
        const Mat4 world = translation({1.0e4f, -1.0e4f, 1.0e4f}) * rotationEuler({0.001f, 0.002f, 0.0f});
        const CameraBasis b = basisFromWorld(world);
        const Vec3 want{-world.at(0, 2), -world.at(1, 2), -world.at(2, 2)};
        QVERIFY2(close(b.forward, want, 1e-6f), qPrintable(show(b.forward)));
        // a negative scale is taken as it is: on z the camera looks the other way, on x it does not (and nothing is mirrored)
        VERIFY_VEC(basisFromWorld(scaling({1.0f, 1.0f, -1.0f})).forward, (Vec3{0, 0, 1}));
        VERIFY_VEC(basisFromWorld(scaling({-2.0f, 1.0f, 1.0f})).forward, (Vec3{0, 0, -1}));
        VERIFY_VEC(basisFromWorld(scaling({-2.0f, 1.0f, 1.0f})).right, (Vec3{1, 0, 0}));
    }

    void eulerAnglesAtAQuarterTurnUpOrDown()
    {
        // looking straight up or down (a pitch of +-90 degrees), and a quarter turn about y (the gimbal case)
        const Vec3 cases[][2] = {{{0, 1, 0}, {0, 0, 1}}, {{0, -1, 0}, {0, 0, -1}}, {{0, 1, 0}, {1, 0, 0}}, {{1, 0, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, -1, 0}}};
        for (const auto &t : cases) {
            const CameraBasis want = lookAtBasis({0, 0, 0}, t[0], t[1]);
            const CameraBasis got = basisFromWorld(rotationEuler(eulerFromBasis(want)));
            VERIFY_VEC(got.forward, want.forward);
            VERIFY_VEC(got.up, want.up);
        }
    }

    void pathsPlayInOrderOrAtRandom()
    {
        CameraPathQueue q;
        // in order: 2 s, a hidden one, 1 s, then from the start again
        q.reset({2.0, 0.0, 1.0}, false);
        QCOMPARE(q.at(10.0).first, 0);
        QCOMPARE(q.at(11.5).first, 0);
        QVERIFY(close(static_cast<float>(q.at(11.5).second), 1.5f));
        QCOMPARE(q.at(12.0).first, 2);
        QCOMPARE(q.at(12.9).first, 2);
        QCOMPARE(q.at(13.0).first, 0);
        // a long pause skips what it missed
        QCOMPARE(q.at(13.0 + 3.0 * 4.0 + 2.5).first, 2);
        // at random: never the same twice running, every playable one comes, the hidden one never
        q.reset({1.0, 1.0, 0.0, 1.0}, true, 7);
        std::set<int> seen;
        int last = -1;
        for (int i = 0; i < 60; ++i) {
            const int path = q.at(0.5 + i).first;
            QVERIFY(path != 2);
            QVERIFY(path != last);
            last = path;
            seen.insert(path);
        }
        QCOMPARE(seen.size(), size_t{3});
        // the same seed plays the same order
        CameraPathQueue a, b2;
        a.reset({1.0, 1.0, 1.0, 1.0, 1.0}, true, 42);
        b2.reset({1.0, 1.0, 1.0, 1.0, 1.0}, true, 42);
        std::vector<int> first, second;
        for (int i = 0; i < 40; ++i) {
            first.push_back(a.at(0.5 + i).first);
            second.push_back(b2.at(0.5 + i).first);
        }
        QVERIFY(first == second);
        // a pause of months in order: whole rounds are skipped at once, and the path is the one the clock says
        q.reset({2.0, 1.0}, false);
        QCOMPARE(q.at(0.0).first, 0);
        const auto late = q.at(3.0e7 + 2.5); // 10^7 rounds of 3 s, then 2.5 s into the round: the second path, 0.5 s in
        QCOMPARE(late.first, 1);
        QVERIFY(std::fabs(late.second - 0.5) < 1e-3);
        // one path plays over and over; none: nothing plays
        q.reset({2.0}, true);
        QCOMPARE(q.at(0.0).first, 0);
        QCOMPARE(q.at(5.0).first, 0);
        QVERIFY(close(static_cast<float>(q.at(5.0).second), 1.0f));
        q.reset({0.0}, false);
        QCOMPARE(q.at(1.0).first, -1);
        q.reset({}, false);
        QCOMPARE(q.at(1.0).first, -1);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkCamera)
#include "tst_scenevk_camera.moc"
