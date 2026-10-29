#include "spansync.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSpanSync : public QObject
{
    Q_OBJECT

private slots:
    void playersWithinTheToleranceAreLeftAlone();
    void aPlayerThatStraysIsSoughtToTheFirstOnes();
    void unknownPositionsTakeNoPart();
    void theReferenceIsTheFirstKnownPlayer();
    void fewerThanTwoPlayersNeedNothing();
    void theToleranceIsConfigurable();
};

void TestSpanSync::playersWithinTheToleranceAreLeftAlone()
{
    QVERIFY(spanVideoCorrections({{u"A"_s, 10.00}, {u"B"_s, 10.04}, {u"C"_s, 9.96}}).isEmpty());
    QVERIFY(spanVideoCorrections({{u"A"_s, 10.00}, {u"B"_s, 10.05}}).isEmpty()); // exactly the tolerance
}

void TestSpanSync::aPlayerThatStraysIsSoughtToTheFirstOnes()
{
    const auto fixes = spanVideoCorrections({{u"A"_s, 10.0}, {u"B"_s, 10.2}, {u"C"_s, 10.01}, {u"D"_s, 9.5}});
    QCOMPARE(fixes.size(), 2);
    QCOMPARE(fixes.at(0).output, u"B"_s);
    QCOMPARE(fixes.at(0).seconds, 10.0);
    QCOMPARE(fixes.at(1).output, u"D"_s);
    QCOMPARE(fixes.at(1).seconds, 10.0);
}

void TestSpanSync::unknownPositionsTakeNoPart()
{
    // -1: nothing plays yet (still loading) or the output is paused
    QVERIFY(spanVideoCorrections({{u"A"_s, -1.0}, {u"B"_s, 3.0}, {u"C"_s, -1.0}}).isEmpty());
    const auto fixes = spanVideoCorrections({{u"A"_s, -1.0}, {u"B"_s, 3.0}, {u"C"_s, 4.0}});
    QCOMPARE(fixes.size(), 1);
    QCOMPARE(fixes.at(0).output, u"C"_s);
    QCOMPARE(fixes.at(0).seconds, 3.0);
}

void TestSpanSync::theReferenceIsTheFirstKnownPlayer()
{
    const auto fixes = spanVideoCorrections({{u"A"_s, 5.0}, {u"B"_s, 1.0}});
    QCOMPARE(fixes.size(), 1);
    QCOMPARE(fixes.at(0).output, u"B"_s); // never A: the first output sets the time
}

void TestSpanSync::fewerThanTwoPlayersNeedNothing()
{
    QVERIFY(spanVideoCorrections({}).isEmpty());
    QVERIFY(spanVideoCorrections({{u"A"_s, 1.0}}).isEmpty());
}

void TestSpanSync::theToleranceIsConfigurable()
{
    QCOMPARE(spanVideoCorrections({{u"A"_s, 1.0}, {u"B"_s, 1.2}}, 0.5).size(), 0);
    QCOMPARE(spanVideoCorrections({{u"A"_s, 1.0}, {u"B"_s, 1.2}}, 0.1).size(), 1);
}

QTEST_APPLESS_MAIN(TestSpanSync)
#include "tst_spansync.moc"
