// Footprint, window rectangle, clamps and hit test of a rotated and flipped pet (src/overlay/petgeometry.h).

#include "petgeometry.h"

#include <QTest>

#include <cmath>
#include <limits>

using namespace murasaki;

class TstPetGeometry : public QObject
{
    Q_OBJECT

private slots:
    void footprintAtRightAngles_data();
    void footprintAtRightAngles();
    void footprintAtAnArbitraryAngle();
    void rotationWraps();
    void windowRectIsCentredOnThePet();
    void ceilPixelsAbsorbNoise();
    void clampKeepsAPartReachable();
    void clampInsideOutput();
    void clampWithAnOriginAndDegenerateRanges();
    void nonFiniteInputsAreHarmless();
    void rotationAboutTheCentre();
    void hitTestUnrotated();
    void hitTestRotated90();
    void hitTestOutsideTheTurnedRectangle();
    void flipsMirrorThePictureBeforeTheRotation();
    void resizingKeepsTheOppositeCornerWhereItWas_data();
    void resizingKeepsTheOppositeCornerWhereItWas();
    void resizingAtZeroKeepsTheTopLeft();
    void aFootprintLargerThanTheOutputIsClampedWithoutAnInvertedRange();
};

void TstPetGeometry::footprintAtRightAngles_data()
{
    QTest::addColumn<double>("degrees");
    QTest::addColumn<double>("w");
    QTest::addColumn<double>("h");
    QTest::newRow("0") << 0.0 << 100.0 << 50.0;
    QTest::newRow("90") << 90.0 << 50.0 << 100.0;
    QTest::newRow("180") << 180.0 << 100.0 << 50.0;
    QTest::newRow("270") << 270.0 << 50.0 << 100.0;
    QTest::newRow("360") << 360.0 << 100.0 << 50.0;
    QTest::newRow("-90") << -90.0 << 50.0 << 100.0;
}

void TstPetGeometry::footprintAtRightAngles()
{
    QFETCH(double, degrees);
    QFETCH(double, w);
    QFETCH(double, h);
    const PetSize f = petFootprint({100, 50}, degrees);
    QCOMPARE(f.w, w); // exact: a window size is a pixel count
    QCOMPARE(f.h, h);
}

void TstPetGeometry::footprintAtAnArbitraryAngle()
{
    const double a = 33.3 * kPetPi / 180.0;
    const PetSize f = petFootprint({100, 50}, 33.3);
    QVERIFY(std::fabs(f.w - (100 * std::cos(a) + 50 * std::sin(a))) < 1e-9);
    QVERIFY(std::fabs(f.h - (100 * std::sin(a) + 50 * std::cos(a))) < 1e-9);
    // 45 degrees of a square is its diagonal.
    const PetSize d = petFootprint({10, 10}, 45);
    QVERIFY(std::fabs(d.w - 10 * std::sqrt(2.0)) < 1e-9);
    QVERIFY(std::fabs(d.h - 10 * std::sqrt(2.0)) < 1e-9);
    // The same bounding box the other way round.
    const PetSize g = petFootprint({100, 50}, 33.3 + 180);
    QVERIFY(std::fabs(g.w - f.w) < 1e-9 && std::fabs(g.h - f.h) < 1e-9);
}

void TstPetGeometry::rotationWraps()
{
    QCOMPARE(normalizeRotation(0), 0.0);
    QCOMPARE(normalizeRotation(360), 0.0);
    QCOMPARE(normalizeRotation(450), 90.0);
    QCOMPARE(normalizeRotation(-90), 270.0);
    QVERIFY(normalizeRotation(-1e-18) < 360.0);
}

void TstPetGeometry::windowRectIsCentredOnThePet()
{
    const PetRect pet{200, 100, 100, 50};
    QCOMPARE(petWindowRect(pet, 0), pet);
    const PetRect turned = petWindowRect(pet, 90); // 50 x 100 around the centre (250, 125)
    QCOMPARE(turned, (PetRect{225, 75, 50, 100}));
    QCOMPARE(turned.center().x, pet.center().x);
    QCOMPARE(turned.center().y, pet.center().y);
    const PetRect diagonal = petWindowRect(pet, 33.3);
    QVERIFY(std::fabs(diagonal.center().x - 250) < 1e-9 && std::fabs(diagonal.center().y - 125) < 1e-9);
    QVERIFY(diagonal.w > pet.w && diagonal.h > pet.h);
}

void TstPetGeometry::ceilPixelsAbsorbNoise()
{
    QCOMPARE(petCeilPx(120.0000000001), 120);
    QCOMPARE(petCeilPx(119.9999999), 120);
    QCOMPARE(petCeilPx(120.2), 121);
    QCOMPARE(petCeilPx(0.0), 1);
}

void TstPetGeometry::clampKeepsAPartReachable()
{
    const PetRect output{0, 0, 1280, 720};
    // Far off to the right and down: 48 px stay inside.
    PetRect r = clampToOutput({5000, 5000, 200, 100}, output);
    QCOMPARE(r.x, 1280.0 - 48);
    QCOMPARE(r.y, 720.0 - 48);
    // Far off to the left and up: 48 px of the far edge stay inside.
    r = clampToOutput({-5000, -5000, 200, 100}, output);
    QCOMPARE(r.x, 48.0 - 200);
    QCOMPARE(r.y, 48.0 - 100);
    // A window smaller than 48 keeps all of it.
    r = clampToOutput({5000, 5000, 20, 10}, output);
    QCOMPARE(r.x, 1280.0 - 20);
    QCOMPARE(r.y, 720.0 - 10);
    // A window already inside is left alone.
    QCOMPARE(clampToOutput({10, 20, 200, 100}, output), (PetRect{10, 20, 200, 100}));
}

void TstPetGeometry::clampInsideOutput()
{
    const PetRect output{0, 0, 1280, 720};
    const PetRect r = murasaki::clampInsideOutput({1200, -30, 200, 100}, output);
    QCOMPARE(r.x, 1080.0);
    QCOMPARE(r.y, 0.0);
    // Larger than the output: sticks to the top-left corner.
    const PetRect big = murasaki::clampInsideOutput({50, 50, 2000, 900}, output);
    QCOMPARE(big.x, 0.0);
    QCOMPARE(big.y, 0.0);
}

void TstPetGeometry::clampWithAnOriginAndDegenerateRanges()
{
    // A second output to the right: the same rule, shifted.
    const PetRect output{1280, 0, 1920, 1080};
    PetRect r = clampToOutput({9000, 0, 200, 100}, output);
    QCOMPARE(r.x, 3200.0 - 48);
    r = clampToOutput({0, 0, 200, 100}, output);
    QCOMPARE(r.x, 1280.0 + 48 - 200);
    // lo > hi (a window and an output both tiny): defined, no precondition violated.
    r = clampToOutput({5, 5, 20, 10}, {0, 0, 10, 5});
    QVERIFY(std::isfinite(r.x) && std::isfinite(r.y));
    r = clampToOutput({5, 5, 20, 10}, {0, 0, 0, 0});
    QVERIFY(std::isfinite(r.x) && std::isfinite(r.y));
    const PetRect inside = murasaki::clampInsideOutput({5, 5, 20, 10}, {100, 50, 10, 5});
    QVERIFY(std::isfinite(inside.x) && std::isfinite(inside.y));
}

void TstPetGeometry::nonFiniteInputsAreHarmless()
{
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    QCOMPARE(normalizeRotation(nan), 0.0);
    QCOMPARE(normalizeRotation(inf), 0.0);
    QCOMPARE(normalizeRotation(-inf), 0.0);
    const PetSize f = petFootprint({100, 50}, nan);
    QCOMPARE(f.w, 100.0);
    QCOMPARE(f.h, 50.0);
    const PetSize z = petFootprint({nan, -5}, 33);
    QCOMPARE(z.w, 0.0);
    QCOMPARE(z.h, 0.0);
    QCOMPARE(petFootprint({-5, 10}, 0).w, 0.0);
    QCOMPARE(petCeilPx(nan), 1);
    QCOMPARE(petCeilPx(inf), 1);
    QCOMPARE(petCeilPx(1e300), 1000000000);
    QCOMPARE(petCeilPx(-4), 1);
    const PetRect pet{0, 0, 100, 50};
    QVERIFY(!pointInPet({nan, 10}, pet, 0, false, false));
    QVERIFY(!pointInPet({10, inf}, pet, 0, false, false));
    QVERIFY(!pointInPet({10, 10}, {nan, 0, 100, 50}, 0, false, false));
    QVERIFY(!pointInPet({10, 10}, {0, 0, 0, 0}, 0, false, false));
    const PetRect c = clampToOutput({nan, inf, 10, 10}, {0, 0, 100, 100});
    QVERIFY(std::isfinite(c.x) && std::isfinite(c.y));
}

void TstPetGeometry::rotationAboutTheCentre()
{
    const PetPoint c{100, 100};
    PetPoint p = rotatePoint({150, 100}, c, 90); // clockwise on screen: the right of the centre goes below it
    QVERIFY(std::fabs(p.x - 100) < 1e-9 && std::fabs(p.y - 150) < 1e-9);
    p = rotatePoint({150, 100}, c, 180);
    QVERIFY(std::fabs(p.x - 50) < 1e-9 && std::fabs(p.y - 100) < 1e-9);
    p = rotatePoint({150, 100}, c, 360);
    QVERIFY(std::fabs(p.x - 150) < 1e-9 && std::fabs(p.y - 100) < 1e-9);
}

void TstPetGeometry::hitTestUnrotated()
{
    const PetRect pet{100, 100, 200, 100};
    const auto in = pointInPet({150, 120}, pet, 0, false, false);
    QVERIFY(in);
    QCOMPARE(in->x, 50.0);
    QCOMPARE(in->y, 20.0);
    QVERIFY(!pointInPet({99, 120}, pet, 0, false, false));
    QVERIFY(!pointInPet({300, 120}, pet, 0, false, false)); // the right edge is outside
    QVERIFY(!pointInPet({150, 200}, pet, 0, false, false));
}

void TstPetGeometry::hitTestRotated90()
{
    // 200 x 100 at (100, 100), centre (200, 150). Turned 90 degrees clockwise it covers x 150..250, y 50..250.
    const PetRect pet{100, 100, 200, 100};
    // The picture's top-left corner (local (0, 0)) goes to the top-right of the turned rectangle.
    const auto corner = pointInPet({249.5, 50.5}, pet, 90, false, false);
    QVERIFY(corner);
    QVERIFY(std::fabs(corner->x - 0.5) < 1e-9 && std::fabs(corner->y - 0.5) < 1e-9);
    // The picture's right edge midpoint (200, 50) goes below the centre.
    const auto right = pointInPet({200, 249}, pet, 90, false, false);
    QVERIFY(right);
    QVERIFY(std::fabs(right->x - 199) < 1e-9 && std::fabs(right->y - 50) < 1e-9);
}

void TstPetGeometry::hitTestOutsideTheTurnedRectangle()
{
    const PetRect pet{100, 100, 200, 100};
    // Inside the unrotated rectangle, outside the turned one.
    QVERIFY(pointInPet({110, 110}, pet, 0, false, false));
    QVERIFY(!pointInPet({110, 110}, pet, 90, false, false));
    // Inside the turned one, outside the unrotated one.
    QVERIFY(!pointInPet({200, 60}, pet, 0, false, false));
    QVERIFY(pointInPet({200, 60}, pet, 90, false, false));
    // Inside the window (the footprint of 33.3 degrees) but in a corner the turned rectangle does not cover.
    const PetRect window = petWindowRect(pet, 33.3);
    QVERIFY(!pointInPet({window.x + 1, window.y + 1}, pet, 33.3, false, false));
    QVERIFY(pointInPet(pet.center(), pet, 33.3, false, false));
}

void TstPetGeometry::flipsMirrorThePictureBeforeTheRotation()
{
    const PetRect pet{0, 0, 100, 50};
    // Unrotated: the point near the left edge shows the right part of a horizontally flipped picture.
    auto p = pointInPet({10, 10}, pet, 0, true, false);
    QVERIFY(p);
    QVERIFY(std::fabs(p->x - 90) < 1e-9 && std::fabs(p->y - 10) < 1e-9);
    p = pointInPet({10, 10}, pet, 0, false, true);
    QVERIFY(p);
    QVERIFY(std::fabs(p->x - 10) < 1e-9 && std::fabs(p->y - 40) < 1e-9);
    p = pointInPet({10, 10}, pet, 0, true, true);
    QVERIFY(p);
    QVERIFY(std::fabs(p->x - 90) < 1e-9 && std::fabs(p->y - 40) < 1e-9);
    // Flip then rotate 180 equals the opposite flip: a horizontal flip turned half a way is a vertical flip.
    const auto a = pointInPet({10, 10}, pet, 180, true, false);
    const auto b = pointInPet({10, 10}, pet, 0, false, true);
    QVERIFY(a && b);
    QVERIFY(std::fabs(a->x - b->x) < 1e-9 && std::fabs(a->y - b->y) < 1e-9);
}

void TstPetGeometry::resizingKeepsTheOppositeCornerWhereItWas_data()
{
    QTest::addColumn<double>("degrees");
    for (const double degrees : {0.0, 90.0, 180.0, 270.0, 37.0, 359.5, 123.456})
        QTest::newRow(qPrintable(QString::number(degrees))) << degrees;
}

void TstPetGeometry::resizingKeepsTheOppositeCornerWhereItWas()
{
    QFETCH(double, degrees);
    const PetRect before{300, 200, 160, 90};
    const PetPoint corner = rotatePoint(before.topLeft(), before.center(), degrees); // on the screen
    for (const PetSize size : {PetSize{320, 180}, PetSize{80, 45}, PetSize{161, 91}, PetSize{16, 9}}) {
        const PetPoint topLeft = petTopLeftKeepingTopLeftCorner(before, size, degrees);
        const PetRect after{topLeft.x, topLeft.y, size.w, size.h};
        const PetPoint now = rotatePoint(after.topLeft(), after.center(), degrees);
        QVERIFY2(std::fabs(now.x - corner.x) < 1e-9 && std::fabs(now.y - corner.y) < 1e-9, qPrintable(QString::number(degrees)));
    }
    // the handle (the bottom-right in the pet's frame) is where the pointer pulls it: the diagonal grows along the turned axes
    const PetSize bigger{320, 180};
    const PetPoint topLeft = petTopLeftKeepingTopLeftCorner(before, bigger, degrees);
    const PetRect after{topLeft.x, topLeft.y, bigger.w, bigger.h};
    const PetPoint handle = rotatePoint({after.right(), after.bottom()}, after.center(), degrees);
    const PetPoint expected{corner.x + (bigger.w * std::cos(degrees * kPetPi / 180) - bigger.h * std::sin(degrees * kPetPi / 180)),
                            corner.y + (bigger.w * std::sin(degrees * kPetPi / 180) + bigger.h * std::cos(degrees * kPetPi / 180))};
    QVERIFY(std::fabs(handle.x - expected.x) < 1e-9 && std::fabs(handle.y - expected.y) < 1e-9);
}

void TstPetGeometry::resizingAtZeroKeepsTheTopLeft()
{
    const PetPoint p = petTopLeftKeepingTopLeftCorner({300, 200, 160, 90}, {400, 225}, 0);
    QCOMPARE(p.x, 300.0);
    QCOMPARE(p.y, 200.0);
    // at 180 degrees the corner is the bottom-right of the picture on the screen: the pet grows to the top-left of it
    const PetPoint q = petTopLeftKeepingTopLeftCorner({300, 200, 160, 90}, {320, 180}, 180);
    QCOMPARE(q.x, 300.0 + 160 - 320);
    QCOMPARE(q.y, 200.0 + 90 - 180);
    // not a number in: a defined answer
    const PetPoint n = petTopLeftKeepingTopLeftCorner({300, 200, 160, 90}, {std::nan(""), 10}, 45);
    Q_UNUSED(n);
}

void TstPetGeometry::aFootprintLargerThanTheOutputIsClampedWithoutAnInvertedRange()
{
    // a 600 x 400 pet turned 33 degrees on a 300 x 200 output: the footprint is far larger than the screen
    const PetRect window = petWindowRect({-100, -50, 600, 400}, 33);
    QVERIFY(window.w > 300 && window.h > 200);
    const PetRect output{0, 0, 300, 200};
    const PetRect kept = clampToOutput(window, output);
    // at least 48 px of the footprint stay inside the output on each axis, wherever it started
    QVERIFY(kept.x <= output.right() - 48 && kept.right() >= output.x + 48);
    QVERIFY(kept.y <= output.bottom() - 48 && kept.bottom() >= output.y + 48);
    for (const PetRect far : {PetRect{-5000, -5000, window.w, window.h}, PetRect{5000, 5000, window.w, window.h}}) {
        const PetRect back = clampToOutput(far, output);
        QVERIFY(back.x <= output.right() - 48 && back.right() >= output.x + 48);
        QVERIFY(back.y <= output.bottom() - 48 && back.bottom() >= output.y + 48);
    }
    // the window size itself is whole pixels that hold the footprint
    QVERIFY(petCeilPx(window.w) >= window.w - 1e-6);
}

QTEST_APPLESS_MAIN(TstPetGeometry)
#include "tst_petgeometry.moc"
