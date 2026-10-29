// The line protocol of murasaki-screenmatch (src/screenmatch/protocol.h): every command and reply round trips, every kind of bad line is refused, the 4 KiB bound holds, and a seeded
// fuzz run over mutated lines never crashes or accepts what it would not format back the same.

#include "screenmatch/protocol.h"

#include <QRandomGenerator>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace murasaki::screenmatch;

namespace {

QByteArray grid(int seed = 1)
{
    QByteArray g(kGridBytes, 0);
    for (int i = 0; i < g.size(); ++i)
        g[i] = char(i * 7 + seed);
    return g;
}

Command command(CommandType type)
{
    Command c;
    c.type = type;
    c.trigger = u"trig-1_A"_s;
    c.mode = ScreenMatchMode::Structure;
    c.region = 3;
    c.stream = 2;
    c.x = 10;
    c.y = 20;
    c.w = 300;
    c.h = 200;
    c.step = 5;
    c.grid = grid();
    c.rate = 7;
    c.request = 42;
    c.pets = {{0, 1, 2, 3, 4}, {1, 100, 200, 300, 400}};
    return c;
}

} // namespace

class TstScreenMatchProtocol : public QObject
{
    Q_OBJECT

private slots:
    void everyCommandRoundTrips_data()
    {
        QTest::addColumn<int>("type");
        for (int t = 0; t <= int(CommandType::Quit); ++t)
            QTest::newRow(qPrintable(commandWord(CommandType(t)))) << t;
    }
    void everyCommandRoundTrips()
    {
        QFETCH(int, type);
        const Command c = command(CommandType(type));
        const QByteArray line = formatCommand(c);
        QVERIFY(line.size() < kMaxLineBytes);
        QString why;
        const auto back = parseCommand(line, &why);
        QVERIFY2(back.has_value(), qPrintable(why));
        QCOMPARE(back->type, c.type);
        QCOMPARE(formatCommand(*back), line);
    }

    void parsedFieldsAreTheOnesSent()
    {
        const auto region = parseCommand("region t_1 3 2 10 20 300 200");
        QVERIFY(region);
        QCOMPARE(region->trigger, u"t_1"_s);
        QCOMPARE(region->region, 3);
        QCOMPARE(region->stream, 2);
        QCOMPARE(region->w, 300);
        const auto ref = parseCommand("ref t 0 63 " + grid(9).toBase64());
        QVERIFY(ref);
        QCOMPARE(ref->grid, grid(9));
        QCOMPARE(ref->step, 63);
        const auto pets = parseCommand("pets 2 0 1 2 3 4 1 5 6 7 8");
        QVERIFY(pets);
        QCOMPARE(pets->pets.size(), 2);
        QCOMPARE(pets->pets.at(1).h, 8);
        QCOMPARE(parseCommand("pets 0")->pets.size(), 0);
        QCOMPARE(parseCommand("trigger a hash")->mode, ScreenMatchMode::Hash);
    }

    void badCommandsAreRefusedByTheirGrammar_data()
    {
        QTest::addColumn<QByteArray>("line");
        const QByteArray ok64 = grid().toBase64();
        const auto row = [](const char *name, const QByteArray &line) { QTest::newRow(name) << line; };
        row("empty", "");
        row("unknown word", "format-disk");
        row("capital", "Clear");
        row("clear with an argument", "clear now");
        row("start with an argument", "start 1");
        row("leading space", " clear");
        row("trailing space", "clear ");
        row("double space", "trigger  a hash");
        row("tab", "trigger\ta hash");
        row("carriage return", "clear\r");
        row("newline inside", "clear\nclear");
        row("nul", QByteArray("clear\0x", 7));
        row("non-ascii", "trigger \xc3\xa9 hash");
        row("trigger without mode", "trigger a");
        row("trigger bad mode", "trigger a orb");
        row("trigger empty id", "trigger  hash");
        row("trigger id with a dot", "trigger a.b hash");
        row("trigger id with a slash", "trigger ../x hash");
        row("trigger id of 65", "trigger " + QByteArray(65, 'a') + " hash");
        row("region short", "region a 0 0 1 2 3");
        row("region long", "region a 0 0 1 2 3 4 5");
        row("region index 8", "region a 8 0 1 2 3 4");
        row("region stream 8", "region a 0 8 1 2 3 4");
        row("region negative", "region a 0 0 -1 2 3 4");
        row("region zero width", "region a 0 0 1 2 0 4");
        row("region zero height", "region a 0 0 1 2 3 0");
        row("region huge width", "region a 0 0 1 2 16385 4");
        row("region huge x", "region a 0 0 65536 2 3 4");
        row("region leading zero", "region a 0 0 01 2 3 4");
        row("region plus sign", "region a 0 0 +1 2 3 4");
        row("region hex", "region a 0 0 0x1 2 3 4");
        row("region float", "region a 0 0 1.5 2 3 4");
        row("region 10 digits", "region a 0 0 1234567890 2 3 4");
        row("ref short grid", "ref a 0 0 " + grid().left(1000).toBase64());
        row("ref long grid", "ref a 0 0 " + (grid() + "x").toBase64());
        row("ref bad base64", "ref a 0 0 " + ok64.left(ok64.size() - 1) + "!");
        row("ref unpadded", "ref a 0 0 " + ok64.left(ok64.size() - 1));
        row("ref step 64", "ref a 0 64 " + ok64);
        row("ref region 8", "ref a 8 0 " + ok64);
        row("want step 64", "want a 64");
        row("want negative", "want a -1");
        row("want no id", "want 3");
        row("pets count mismatch", "pets 2 0 1 2 3 4");
        row("pets extra", "pets 1 0 1 2 3 4 5");
        row("pets 65", "pets 65");
        row("pets bad stream", "pets 1 9 1 2 3 4");
        row("pets zero size", "pets 1 0 1 2 0 4");
        row("pets no count", "pets");
        row("rate 0", "rate 0");
        row("rate 17", "rate 17");
        row("rate text", "rate fast");
        row("capture short", "capture 1 0 1 2 3");
        row("capture zero request", "capture 0 0 1 2 3 4");
        row("capture bad stream", "capture 1 8 1 2 3 4");
        row("capture zero size", "capture 1 0 1 2 0 4");
        row("ping extra", "ping ping");
        row("quit extra", "quit now");
        row("over the line limit", "pets 0 " + QByteArray(5000, 'a'));
        row("exactly 4096", QByteArray(4096, 'a'));
    }
    void badCommandsAreRefusedByTheirGrammar()
    {
        QFETCH(QByteArray, line);
        QString why;
        QVERIFY2(!parseCommand(line, &why).has_value(), line.left(60).constData());
        QVERIFY(!why.isEmpty());
    }

    void aReasonNeverEchoesTheLine()
    {
        QString why;
        QVERIFY(!parseCommand("trigger SECRET-TOKEN-12345 orb", &why));
        QVERIFY2(!why.contains(u"SECRET"_s), qPrintable(why));
        QVERIFY(!parseCommand("secretword", &why));
        QVERIFY2(!why.contains(u"secretword"_s), qPrintable(why));
    }

    void theLargestLinesFit()
    {
        Command pets = command(CommandType::Pets);
        pets.pets.clear();
        for (int i = 0; i < kMaxPets; ++i)
            pets.pets.append({7, 65535, 65535, 16384, 16384});
        QVERIFY(formatCommand(pets).size() < kMaxLineBytes);
        QVERIFY(parseCommand(formatCommand(pets)).has_value());
        QVERIFY(formatCommand(command(CommandType::Ref)).size() < kMaxLineBytes);
        Reply chunk;
        chunk.type = ReplyType::Chunk;
        chunk.request = 1000000;
        chunk.sequence = 1000;
        chunk.data = QByteArray(kChunkBytes, '\xff');
        QVERIFY(formatReply(chunk).size() < kMaxLineBytes);
        Reply scores;
        scores.type = ReplyType::Scores;
        scores.trigger = QString(64, u'a');
        scores.step = 63;
        for (int i = 0; i < kMaxRegionsPerTrigger; ++i)
            scores.permille.append(1000);
        QVERIFY(formatReply(scores).size() < kMaxLineBytes);
    }

    void everyReplyRoundTrips()
    {
        const auto trip = [](const Reply &r) {
            const QByteArray line = formatReply(r);
            QString why;
            const auto back = parseReply(line, &why);
            QVERIFY2(back.has_value(), qPrintable(line + ": " + why));
            QCOMPARE(formatReply(*back), line);
        };
        Reply r;
        r.type = ReplyType::Hello;
        r.version = 1;
        trip(r);
        r = {};
        r.type = ReplyType::Ok;
        r.word = u"trigger"_s;
        trip(r);
        r = {};
        r.type = ReplyType::Err;
        r.word = u"region"_s;
        r.reason = u"too-many"_s;
        trip(r);
        r = {};
        r.type = ReplyType::Scores;
        r.trigger = u"abc"_s;
        r.step = 2;
        r.permille = {0, 1000, -1, 515};
        trip(r);
        r = {};
        r.type = ReplyType::Tick;
        r.tick = 123456;
        trip(r);
        for (const char *s : {"waiting", "watching", "stopped"}) {
            r = {};
            r.type = ReplyType::State;
            r.word = QString::fromLatin1(s);
            trip(r);
        }
        r = {};
        r.type = ReplyType::Shot;
        r.request = 9;
        r.width = 10;
        r.height = 20;
        r.bytes = 600;
        trip(r);
        r = {};
        r.type = ReplyType::Chunk;
        r.request = 9;
        r.sequence = 3;
        r.data = QByteArray("some bytes");
        trip(r);
        r = {};
        r.type = ReplyType::ShotEnd;
        r.request = 9;
        trip(r);
        r = {};
        r.type = ReplyType::Pong;
        trip(r);
    }

    void badRepliesAreRefused_data()
    {
        QTest::addColumn<QByteArray>("line");
        for (const char *l : {"", "hello", "hello x", "hello 1 2", "ok", "err x", "s a 0", "s a 64 1", "s a 0 1001", "s a 0 -5", "s a 0 1 2 3 4 5 6 7 8 9", "s ../x 0 1", "t", "t x", "t -1",
                              "state sleeping", "shot 1 361 1 1083", "shot 1 10 10 301", "shot 0 1 1 3", "chunk 1 0", "chunk 1 0 !!!!", "chunk 1 1001 AAAA",
                              "shotend", "shotend 0", "pong now", "format c:", "S a 0 1", "t 1 2"})
            QTest::newRow(l[0] ? l : "empty") << QByteArray(l);
        QTest::newRow("long chunk") << (QByteArray("chunk 1 0 ") + QByteArray(kChunkBytes + 3, '\0').toBase64());
    }
    void badRepliesAreRefused()
    {
        QFETCH(QByteArray, line);
        QVERIFY2(!parseReply(line).has_value(), line.left(50).constData());
    }

    void theLineBufferCutsLinesAndBoundsThem()
    {
        LineBuffer b;
        QCOMPARE(b.feed("clear\nsta", 9), (QList<QByteArray>{"clear"}));
        QCOMPARE(b.feed("rt\n\nping\n", 9), (QList<QByteArray>{"start", "", "ping"}));
        QCOMPARE(b.pending(), qsizetype(0));
        // 1-byte feeds give the same lines
        LineBuffer one;
        QList<QByteArray> got;
        const QByteArray stream = "a b\ncd\n\nlast\n";
        for (char c : stream)
            got += one.feed(&c, 1);
        QCOMPARE(got, (QList<QByteArray>{"a b", "cd", "", "last"}));
        // a line of 4095 bytes is the longest; 4096 bytes (4097 with the newline) is dropped to its newline and counted
        LineBuffer l;
        QCOMPARE(l.feed(QByteArray(4095, 'a') + '\n', 4096).size(), 1);
        QCOMPARE(l.overlong(), 0);
        QCOMPARE(l.feed(QByteArray(4096, 'a') + '\n', 4097).size(), 0);
        QCOMPARE(l.overlong(), 1);
        QCOMPARE(l.feed("ping\n", 5), (QList<QByteArray>{"ping"})); // the next line is fine
        // an endless line costs bounded memory
        LineBuffer big;
        for (int i = 0; i < 1000; ++i)
            big.feed(QByteArray(10000, 'x').constData(), 10000);
        QVERIFY(big.pending() < kMaxLineBytes);
        QCOMPARE(big.overlong(), 1);
        QCOMPARE(big.feed("\nok\n", 4), (QList<QByteArray>{"ok"}));
    }

    void mutatedLinesNeverCrashAndAcceptedOnesAreCanonical()
    {
        QRandomGenerator rng(20261023);
        QList<QByteArray> seeds;
        for (int t = 0; t <= int(CommandType::Quit); ++t)
            seeds << formatCommand(command(CommandType(t)));
        int accepted = 0, refused = 0;
        for (int i = 0; i < 6000; ++i) {
            QByteArray line = seeds.at(i % seeds.size());
            for (int e = 0, n = 1 + int(rng.bounded(4)); e < n && !line.isEmpty(); ++e) {
                switch (rng.bounded(5)) {
                case 0: line[int(rng.bounded(line.size()))] = char(rng.bounded(256)); break;
                case 1: line.truncate(int(rng.bounded(line.size() + 1))); break;
                case 2: line.insert(int(rng.bounded(line.size() + 1)), QByteArray(1 + int(rng.bounded(3)), char(rng.bounded(256)))); break;
                case 3: line.remove(int(rng.bounded(line.size())), 1 + int(rng.bounded(3))); break;
                case 4: line.insert(int(rng.bounded(line.size() + 1)), ' '); break;
                }
            }
            const auto c = parseCommand(line);
            if (!c) {
                ++refused;
                continue;
            }
            ++accepted;
            // what was accepted formats back to the same bytes (the grammar has one spelling; base64 of the same bytes is unique when padded)
            QVERIFY2(formatCommand(*c) == line, line.left(80).constData());
        }
        qInfo() << "fuzz:" << accepted << "accepted," << refused << "refused";
        QVERIFY(accepted > 100 && refused > 3000);
        // replies too
        for (int i = 0; i < 3000; ++i) {
            QByteArray line = "s abc 2 100 -  500";
            line[int(rng.bounded(line.size()))] = char(rng.bounded(256));
            parseReply(line);
        }
    }
};

QTEST_GUILESS_MAIN(TstScreenMatchProtocol)
#include "tst_screenmatchprotocol.moc"
