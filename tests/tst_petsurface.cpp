// The pure rules of a pet window: its flags, how far it may be scaled and how large it is shown.

#include "petsurface.h"

#include <QTest>

using namespace murasaki;

class TstPetSurface : public QObject
{
    Q_OBJECT

private slots:
    void clickThroughUnlessArranging();
    void widthIsClampedToTheScaleRange_data();
    void widthIsClampedToTheScaleRange();
    void heightFollowsTheAspectRatio();
    void aStageNeedsNoMasksWhateverElseItSays();
};

void TstPetSurface::clickThroughUnlessArranging()
{
    const Qt::WindowFlags idle = petWindowFlags(false);
    QVERIFY(idle.testFlag(Qt::WindowTransparentForInput));
    QVERIFY(idle.testFlag(Qt::FramelessWindowHint));
    QCOMPARE(idle & Qt::WindowType_Mask, Qt::WindowFlags(Qt::Window)); // a top-level window, not a Qt::Widget
    QVERIFY(idle.testFlag(Qt::WindowDoesNotAcceptFocus));
    const Qt::WindowFlags arranging = petWindowFlags(true);
    QVERIFY(!arranging.testFlag(Qt::WindowTransparentForInput));
    QVERIFY(arranging.testFlag(Qt::WindowDoesNotAcceptFocus)); // never steals the keyboard either
    QCOMPARE(idle & ~Qt::WindowFlags(Qt::WindowTransparentForInput), arranging);
}

void TstPetSurface::widthIsClampedToTheScaleRange_data()
{
    QTest::addColumn<double>("width");
    QTest::addColumn<int>("native");
    QTest::addColumn<double>("expected");
    QTest::newRow("inside") << 300.0 << 400 << 300.0;
    QTest::newRow("tiny") << 1.0 << 400 << 40.0;         // 0.1x
    QTest::newRow("huge") << 5000.0 << 400 << 800.0;     // 2x
    QTest::newRow("daemon minimum") << 1.0 << 50 << 16.0; // 0.1x would be 5 px
    QTest::newRow("exactly native") << 400.0 << 400 << 400.0;
    QTest::newRow("huge native") << 99999.0 << 20000 << 16384.0; // the daemon's maximum
}

void TstPetSurface::widthIsClampedToTheScaleRange()
{
    QFETCH(double, width);
    QFETCH(int, native);
    QFETCH(double, expected);
    QCOMPARE(clampPetWidth(width, native), expected);
}

void TstPetSurface::heightFollowsTheAspectRatio()
{
    QCOMPARE(petDisplaySize(200, QSize(400, 300)), QSize(200, 150));
    QCOMPARE(petDisplaySize(100, QSize(100, 400)), QSize(100, 400));
    QCOMPARE(petDisplaySize(0.2, QSize(400, 300)), QSize(1, 1)); // never an empty window
    QCOMPARE(petDisplaySize(64, QSize()), QSize(64, 64));        // size not known yet
}

void TstPetSurface::aStageNeedsNoMasksWhateverElseItSays()
{
    PetPlacement p;
    p.clickThrough = false;
    p.hoverHide = true;
    QVERIFY(needsMasks(p)); // an ordinary pet with these has a use for its masks
    p.stage.mode = StageMode::Output;
    QVERIFY(!needsMasks(p)); // a stage takes no pointer: no mask pass, no decode of masks, no cache file
    p.stage.mode = StageMode::Desktop;
    QVERIFY(!needsMasks(p));
    p.stage.mode = StageMode::Off;
    QVERIFY(needsMasks(p));
    // the view of an ordinary pet is not a stage
    PetView view;
    QVERIFY(!view.stage.active);
}

QTEST_APPLESS_MAIN(TstPetSurface)
#include "tst_petsurface.moc"
