// The .mdaa action table parser (src/import/mddoparser.h): JSON first, then the closed literal grammar, and every refusal.

#include "mddoparser.h"
#include "petimport.h"

#include <QRandomGenerator>
#include <QTest>

using namespace murasaki::import;

namespace {

std::optional<MddoAction> one(const char *line, QString *reason = nullptr)
{
    QString local;
    return parseMddoLine(line, 1, reason ? reason : &local);
}

} // namespace

class TstMddoParser : public QObject
{
    Q_OBJECT

private slots:
    void jsonLinesAreRead()
    {
        const auto a = one(R"j([1, "walk.gif", "1-(120, 340)"])j");
        QVERIFY(a);
        QVERIFY(a->show);
        QCOMPARE(a->path, QStringLiteral("walk.gif"));
        QVERIFY(a->hasPosition);
        QCOMPARE(a->x, 120.0);
        QCOMPARE(a->y, 340.0);
        const auto hide = one(R"j([0, "walk.gif", "1-(0,0)"])j");
        QVERIFY(hide && !hide->show);
    }
    void pythonLiteralsAreRead()
    {
        QVERIFY(one("[1, 'a.png', '1-(10, 20)']"));
        const auto t = one("(1, 'a.png', (10, 20))");
        QVERIFY2(t, "tuple with a tuple position");
        QCOMPARE(t->x, 10.0);
        const auto l = one("[0, \"dir\\\\a b.png\", [5.5, -6.25]]");
        QVERIFY(l);
        QCOMPARE(l->path, QStringLiteral("dir\\a b.png")); // the backslash is kept as written
        QCOMPARE(l->x, 5.5);
        QCOMPARE(l->y, -6.25);
        QVERIFY(one("[1, 'a', (1,2),]")); // trailing comma
        QVERIFY(one("[1, 'caf\\xe9', '1-(1,2)']"));
        QVERIFY(one("[1, 'x\\u00e9y', '1-(1,2)']"));
    }
    void extraCoordinatesAreIgnored()
    {
        const auto a = one("[1, 'a', '1-(1,2,3)']"); // only x and y are used
        QVERIFY(a);
        QCOMPARE(a->y, 2.0);
    }
    void aPositionOtherThanOneMeansNoMove()
    {
        const auto a = one(R"j([1, "a.png", "2-(10, 20)"])j");
        QVERIFY(a);
        QVERIFY(!a->hasPosition);
    }
    void refusalsCarryAReason()
    {
        const char *bad[] = {
            "",
            "[]",
            "[1]",
            "[1, 'a']",
            "[1, 'a', '1-(1,2)', 4]",
            "[2, 'a', '1-(1,2)']",
            "[-1, 'a', '1-(1,2)']",
            "[True, 'a', '1-(1,2)']",
            "[1.0, 'a', '1-(1,2)']",
            "['1', 'a', '1-(1,2)']",
            "[1, '', '1-(1,2)']",
            "[1, 5, '1-(1,2)']",
            "[1, 'a', 'bogus']",
            "[1, 'a', '1-(1)']",
            "[1, 'a', (1,)]",
            "[1, 'a', '1-(1e9,2)']",
            "[1, 'a', '1-(nan,2)']",
            "[1, 'a', '1-(inf,2)']",
            "[1, 'a', '1-(-1000000,2)']",
            "__import__('os').system('x')",
            "[1, open('f').read(), '1-(1,2)']",
            "[1, 'a' 'b', '1-(1,2)']",
            "[1, 'a' + 'b', '1-(1,2)']",
            "[1, f'a', '1-(1,2)']",
            "[1, r'a', '1-(1,2)']",
            "[1, b'a', '1-(1,2)']",
            "[1, '''a''', '1-(1,2)']",
            "[1, 'a', '1-(1,2)'] # trailing",
            "[1, 'a', '1-(1,2)'] extra",
            "[1, a, '1-(1,2)']",
            "[1, 'a', '1-(1,2)'",
            "[1, 'a', '1-(1,2)']]",
            "{1: 'a'}",
            "[1, 'a\\q', '1-(1,2)']",
            "[1, 'a\\x4', '1-(1,2)']",
            "[1, 'unterminated, '1-(1,2)']",
            "[1, 0x10, '1-(1,2)']",
            "[1, 1_0, '1-(1,2)']",
            "[1, 'a', None]",
            "None",
            "'just a string'",
            "42",
        };
        for (const char *line : bad) {
            QString reason;
            QVERIFY2(!one(line, &reason), line);
            QVERIFY2(!reason.isEmpty(), line);
        }
    }
    void controlsAndOctalEscapesInAPathAreRefused()
    {
        for (const char *line : {R"j([1, "a\u0000b", "1-(1,2)"])j", R"j([1, 'a\x00b', '1-(1,2)'])j", R"j([1, 'a\u0007b', '1-(1,2)'])j", R"j([1, 'a\x7fb', '1-(1,2)'])j",
                                 R"j([1, 'a\0b', '1-(1,2)'])j", R"j([1, 'a\012b', '1-(1,2)'])j", R"j([1, 'a\ud800b', '1-(1,2)'])j", R"j([1, "a\ud800b", "1-(1,2)"])j"}) {
            QString reason;
            QVERIFY2(!one(line, &reason), line);
            QVERIFY2(!reason.isEmpty(), line);
        }
        QVERIFY(one(R"j([1, 'caf\xe9', '1-(1,2)'])j")); // a letter is fine
        QVERIFY(one(R"j([1, "tab\there", "1-(1,2)"])j") == std::nullopt); // a tab is a control too
    }
    void jsonAndLiteralShareTheNestingLimit()
    {
        QString reason;
        QVERIFY(!one(R"j([1, "a", [[[[1,2]]]]])j", &reason)); // JSON, five lists deep
        QVERIFY(!one("[1, 'a', [[[[1,2]]]]]", &reason));      // literal, the same
        QVERIFY(one(R"j([1, "a", [1,2]])j"));
        QVERIFY(one("[1, 'a', [1,2]]"));
    }
    void exactlyTheMaximumNumberOfLinesIsAccepted()
    {
        MddoLimits limits;
        limits.maxLines = 3;
        QVERIFY(parseMddo("\n\n\n", limits).ok());     // three lines, then nothing
        QVERIFY(parseMddo("\n\n\n\n", limits).ok() == false); // four lines
        QVERIFY(parseMddo("[1,'a','1-(0,0)']\n[1,'a','1-(0,0)']\n[1,'a','1-(0,0)']\n", limits).ok());
        QVERIFY(parseMddo("\n\n[1,'a','1-(0,0)']", limits).ok());
    }
    void depthItemsAndStringLimitsHold()
    {
        QString reason;
        QVERIFY(!one("[1, 'a', [[[[[[1]]]]]]]", &reason));
        QByteArray many = "[1, 'a', [";
        for (int i = 0; i < 200; ++i)
            many += "1,";
        many += "2]]";
        QVERIFY(!one(many, &reason));
        QByteArray longString = "[1, '" + QByteArray(2000, 'a') + "', '1-(1,2)']";
        QVERIFY(!one(longString, &reason));
        QByteArray deep(5000, '[');
        QVERIFY(!one(deep, &reason)); // no recursion blow-up
        QVERIFY(!one(QByteArray("[1, 'a\0b', '1-(1,2)']", 21), &reason)); // NUL in the path
    }
    void tableKeepsTheGoodLinesAndListsTheBadOnes()
    {
        const QByteArray file = "\xef\xbb\xbf[1, \"a.gif\", \"1-(1,2)\"]\r\n\r\nnot a line\n[0, 'b.gif', '1-(3,4)']\n\n[9, 'c', 'x']\n";
        const MddoTable table = parseMddo(file);
        QVERIFY(table.ok());
        QCOMPARE(table.actions.size(), 2);
        QCOMPARE(table.actions.at(0).line, 1);
        QCOMPARE(table.actions.at(1).line, 4);
        QCOMPARE(table.refused.size(), 2);
        QCOMPARE(table.refused.at(0).line, 3);
        QCOMPARE(table.refused.at(1).line, 6);
    }
    void fileLevelLimitsRefuseTheWholeTable()
    {
        MddoLimits limits;
        limits.maxFileBytes = 50;
        MddoTable t = parseMddo(QByteArray(100, ' '), limits);
        QVERIFY(!t.ok());
        QVERIFY(t.error.startsWith(QString::fromUtf8(kMdaaTableRefused.data(), qsizetype(kMdaaTableRefused.size()))));
        QVERIFY(t.actions.isEmpty());
        MddoLimits lines;
        lines.maxLines = 3;
        t = parseMddo("\n\n\n\n\n", lines);
        QVERIFY(!t.ok());
        MddoLimits lineBytes;
        lineBytes.maxLineBytes = 20;
        t = parseMddo("[1, 'a', '1-(1,2)'] and a very long tail\n[1,'a',(1,2)]\n", lineBytes);
        QVERIFY(t.ok()); // a long line is one refused line, not the file
        QCOMPARE(t.refused.size(), 1);
        QVERIFY(parseMddo("").ok());
        QVERIFY(parseMddo("").actions.isEmpty());
    }
    void mutatedLinesNeverCrash()
    {
        // fixed seed; whatever comes out must be a clean accept or a refusal with a reason
        const QByteArray base = "(1, 'dir\\\\walk.gif', '1-(120, 340)')";
        const QByteArray alphabet = "()[],'\"\\ 0123456789.-+eEaxuTrueFalsNo\n\t#_:{}";
        QRandomGenerator rng(20261022);
        for (int round = 0; round < 20000; ++round) {
            QByteArray line = base;
            const int edits = 1 + int(rng.bounded(5));
            for (int i = 0; i < edits; ++i) {
                const int pos = int(rng.bounded(quint32(line.size() + 1)));
                switch (rng.bounded(3)) {
                case 0:
                    line.insert(pos, alphabet.at(int(rng.bounded(quint32(alphabet.size())))));
                    break;
                case 1:
                    if (pos < line.size())
                        line.remove(pos, 1);
                    break;
                default:
                    if (pos < line.size())
                        line[pos] = char(rng.bounded(256));
                }
            }
            QString reason;
            const auto action = parseMddoLine(line, 1, &reason);
            if (!action)
                QVERIFY(!reason.isEmpty());
            else
                QVERIFY(action->x >= -100000 && action->x <= 100000 && action->y >= -100000 && action->y <= 100000);
        }
    }
};

QTEST_GUILESS_MAIN(TstMddoParser)
#include "tst_mddoparser.moc"
