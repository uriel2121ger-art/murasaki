// Sprite sheet geometry (src/import/sheetslicer.h): Anima's defaults, padding and spacing, and every refusal by name.

#include "petimport.h"
#include "sheetslicer.h"

#include <QTest>

#include <limits>

using namespace murasaki::import;

namespace {

SheetSpec spec(int w, int h)
{
    SheetSpec s;
    s.sheetWidth = w;
    s.sheetHeight = h;
    return s;
}

QString name(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

} // namespace

class TstSheetSlicer : public QObject
{
    Q_OBJECT

private slots:
    void animasDefaultsCutFourCellsOfSixtyFour()
    {
        SheetSpec s = spec(256, 64); // 64 x 64, 4 frames, from frame 1, no padding
        QCOMPARE(s.frameWidth, 64);
        QCOMPARE(s.frameHeight, 64);
        QCOMPARE(s.frameCount, 4);
        QCOMPARE(s.startFrame, 1);
        QCOMPARE(s.frameMs, 100);
        const SheetLayout layout = sliceSheet(s);
        QVERIFY2(layout.ok(), qPrintable(layout.error));
        QCOMPARE(layout.columns, 4);
        QCOMPARE(layout.rows, 1);
        QCOMPARE(layout.cells, (QList<SheetCell>{{0, 0}, {64, 0}, {128, 0}, {192, 0}}));
    }
    void cellsRunRowMajorAndWrapAtTheEdge()
    {
        SheetSpec s = spec(128, 128);
        s.frameCount = 4;
        const SheetLayout layout = sliceSheet(s);
        QVERIFY(layout.ok());
        QCOMPARE(layout.cells, (QList<SheetCell>{{0, 0}, {64, 0}, {0, 64}, {64, 64}}));
    }
    void theStartFrameIsOneBased()
    {
        SheetSpec s = spec(128, 128);
        s.frameCount = 2;
        s.startFrame = 3;
        QCOMPARE(sliceSheet(s).cells, (QList<SheetCell>{{0, 64}, {64, 64}}));
        s.startFrame = 4;
        s.frameCount = 1;
        QCOMPARE(sliceSheet(s).cells, (QList<SheetCell>{{64, 64}}));
    }
    void paddingMovesTheOriginAndSpacingTheStep()
    {
        SheetSpec s = spec(10 + 3 * 16 + 2 * 4, 5 + 2 * 20 + 6); // padding 10 / 5, spacing 4 / 6
        s.frameWidth = 16;
        s.frameHeight = 20;
        s.frameCount = 6;
        s.paddingLeft = 10;
        s.paddingTop = 5;
        s.spacingHorizontal = 4;
        s.spacingVertical = 6;
        const SheetLayout layout = sliceSheet(s);
        QVERIFY2(layout.ok(), qPrintable(layout.error));
        QCOMPARE(layout.columns, 3);
        QCOMPARE(layout.rows, 2);
        QCOMPARE(layout.cells.first(), (SheetCell{10, 5}));
        QCOMPARE(layout.cells.at(1), (SheetCell{10 + 16 + 4, 5}));
        QCOMPARE(layout.cells.at(3), (SheetCell{10, 5 + 20 + 6}));
        QCOMPARE(layout.cells.last(), (SheetCell{10 + 2 * 20, 5 + 26}));
    }
    void columnsAndRowsCanBeNarrowedNotWidened()
    {
        SheetSpec s = spec(256, 128);
        s.frameCount = 4;
        s.columns = 2; // a sheet that is wider than the grid the author drew
        QCOMPARE(sliceSheet(s).cells, (QList<SheetCell>{{0, 0}, {64, 0}, {0, 64}, {64, 64}}));
        s.columns = 99;
        QCOMPARE(sliceSheet(s).columns, 4);
        s.columns = 0;
        s.rows = 1;
        s.frameCount = 5;
        QCOMPARE(sliceSheet(s).error.left(name(kSheetTooFewCells).size()), name(kSheetTooFewCells));
    }
    void aCellThatDoesNotFitIsAnErrorNotACrop()
    {
        SheetSpec s = spec(100, 64); // 64 fits once, the second would be cut
        s.frameCount = 2;
        const SheetLayout layout = sliceSheet(s);
        QVERIFY(!layout.ok());
        QVERIFY(layout.error.startsWith(name(kSheetTooFewCells)));
        QVERIFY(layout.error.contains(QStringLiteral("holds 1")));
    }
    void everyRefusalHasItsName()
    {
        const auto refused = [](SheetSpec s, std::string_view wanted) {
            const SheetLayout layout = sliceSheet(s);
            QVERIFY2(!layout.ok(), "accepted");
            QVERIFY2(layout.error.startsWith(name(wanted)), qPrintable(layout.error));
            QVERIFY(layout.cells.isEmpty());
        };
        SheetSpec zero = spec(256, 64);
        zero.frameWidth = 0;
        refused(zero, kSheetFrameIsZero);
        zero.frameWidth = 64;
        zero.frameHeight = -3;
        refused(zero, kSheetFrameIsZero);
        refused(spec(32, 256), kSheetFrameLargerThanSheet);
        refused(spec(256, 32), kSheetFrameLargerThanSheet);
        SheetSpec padded = spec(256, 64);
        padded.paddingLeft = 300;
        refused(padded, kSheetPaddingOutside);
        padded.paddingLeft = -1;
        refused(padded, kSheetPaddingOutside);
        padded.paddingLeft = 0;
        padded.spacingVertical = -5;
        refused(padded, kSheetPaddingOutside);
        SheetSpec many = spec(10000, 10000);
        many.frameWidth = 1;
        many.frameHeight = 1;
        many.frameCount = murasaki::import::kMaxFrames + 1;
        refused(many, kSheetTooManyFrames);
        many.frameCount = 0;
        refused(many, kSheetTooManyFrames);
        SheetSpec start = spec(256, 64);
        start.startFrame = 0;
        refused(start, kSheetBadStart);
        start.startFrame = 5;
        refused(start, kSheetBadStart);
        start.startFrame = 3;
        start.frameCount = 3;
        refused(start, kSheetTooFewCells);
        refused(spec(0, 64), kSheetTooLarge);
        refused(spec(16385, 64), kSheetTooLarge);
        refused(spec(16384, 16384), kSheetTooLarge); // 268 megapixels
        refused(spec(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()), kSheetTooLarge);
        SheetSpec huge = spec(256, 64);
        huge.frameWidth = kMaxCanvasSide + 1;
        refused(huge, kSheetFrameLargerThanSheet);
    }
    void arithmeticDoesNotOverflow()
    {
        SheetSpec s = spec(16384, 6000);
        s.frameWidth = 2;
        s.frameHeight = 2;
        s.frameCount = 20000;
        s.startFrame = std::numeric_limits<int>::max();
        QVERIFY(!sliceSheet(s).ok());
        s.startFrame = 1;
        s.spacingHorizontal = std::numeric_limits<int>::max();
        s.spacingVertical = std::numeric_limits<int>::max();
        const SheetLayout layout = sliceSheet(s);
        QVERIFY(!layout.ok() || layout.cells.size() == 20000); // either refused or exact; never a wrapped coordinate
        for (const SheetCell &cell : layout.cells) {
            QVERIFY(cell.x >= 0 && cell.y >= 0);
            QVERIFY(cell.x + 2 <= 16384 && cell.y + 2 <= 6000);
        }
    }
    void aTwentyThousandCellSheetIsCutExactly()
    {
        SheetSpec s = spec(16384, 6000);
        s.frameWidth = 8;
        s.frameHeight = 8;
        s.frameCount = 20000;
        const SheetLayout layout = sliceSheet(s);
        QVERIFY2(layout.ok(), qPrintable(layout.error));
        QCOMPARE(layout.columns, 2048);
        QCOMPARE(layout.cells.last(), (SheetCell{(19999 % 2048) * 8, (19999 / 2048) * 8}));
        QCOMPARE(layout.cells.size(), 20000);
    }
};

QTEST_GUILESS_MAIN(TstSheetSlicer)
#include "tst_sheetslicer.moc"
