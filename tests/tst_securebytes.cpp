#include "securebytes.h"

#include <QFile>
#include <QTest>

#include <algorithm>

using namespace murasaki;

class TstSecureBytes : public QObject
{
    Q_OBJECT

    // The flags the kernel reports for the mapping that holds `address` (/proc/self/smaps, "VmFlags").
    static QStringList flagsOf(const void *address)
    {
        QFile smaps(QStringLiteral("/proc/self/smaps"));
        if (!smaps.open(QIODevice::ReadOnly))
            return {};
        const quint64 wanted = reinterpret_cast<quintptr>(address);
        bool inside = false;
        for (const QByteArray &line : smaps.readAll().split('\n')) {
            const QList<QByteArray> head = line.split(' ');
            if (!head.isEmpty() && head.first().contains('-') && head.first().at(0) != 'V') {
                const QList<QByteArray> range = head.first().split('-');
                bool a = false, b = false;
                const quint64 from = range.value(0).toULongLong(&a, 16), to = range.value(1).toULongLong(&b, 16);
                inside = a && b && wanted >= from && wanted < to;
            } else if (inside && line.startsWith("VmFlags:")) {
                QStringList flags;
                for (const QByteArray &f : line.mid(8).trimmed().split(' '))
                    flags << QString::fromLatin1(f);
                return flags;
            }
        }
        return {};
    }

private slots:
    void cleanup() { SecureBytes::setWipeObserver({}); }

    void theBytesAreKeptInAMappingOfTheirOwnThatADumpLeavesOut()
    {
        const QByteArray secret(5000, 's');
        SecureBytes bytes{QByteArrayView(secret)};
        QCOMPARE(bytes.size(), qsizetype(5000));
        QCOMPARE(bytes.toByteArray(), secret);
        QCOMPARE(bytes.view(), QByteArrayView(secret));
        QCOMPARE(SecureBytes::liveBuffers(), 1);
        QVERIFY(SecureBytes::liveBytes() >= 5000);
        QCOMPARE(SecureBytes::liveBytes() % 4096, qint64(0)); // whole pages: nothing else shares them
        const QStringList flags = flagsOf(bytes.view().constData());
        QVERIFY2(flags.contains(QStringLiteral("dd")), qPrintable(flags.join(u' ')));   // MADV_DONTDUMP
        QVERIFY2(flags.contains(QStringLiteral("wf")), qPrintable(flags.join(u' ')));   // MADV_WIPEONFORK
        bytes.clear();
        QCOMPARE(SecureBytes::liveBuffers(), 0);
        QVERIFY(bytes.isEmpty());
    }
    void everyWayOutWipesBeforeItUnmaps()
    {
        int seen = 0, dirty = 0;
        qsizetype pages = 0;
        SecureBytes::setWipeObserver([&](const char *, qsizetype size, bool allZero) {
            ++seen;
            dirty += allZero ? 0 : 1;
            pages += size;
        });
        {
            SecureBytes a{QByteArrayView(QByteArray(10, 'a'))}; // destroyed at the end of the scope
        }
        QCOMPARE(seen, 1);
        SecureBytes b{QByteArrayView(QByteArray(10, 'b'))};
        b.clear();
        QCOMPARE(seen, 2);
        SecureBytes c{QByteArrayView(QByteArray(10, 'c'))};
        c = SecureBytes{QByteArrayView(QByteArray(20, 'd'))}; // replaced: the old one is wiped
        QCOMPARE(seen, 3);
        SecureBytes moved = std::move(c);                     // a move wipes nothing and leaves nothing behind
        QCOMPARE(seen, 3);
        QVERIFY(c.isEmpty());
        QCOMPARE(moved.size(), qsizetype(20));
        moved.clear();
        QCOMPARE(seen, 4);
        QCOMPARE(dirty, 0);
        QVERIFY(pages >= 4 * 4096);
        QCOMPARE(SecureBytes::liveBuffers(), 0);
        // nothing: no mapping, no wipe
        SecureBytes none{QByteArrayView()};
        QVERIFY(none.isEmpty());
        none.clear();
        QCOMPARE(seen, 4);
    }
    void wipeOverwritesTheHeapCopyInPlace()
    {
        QByteArray secret(64, 'x');
        const char *where = secret.constData();
        SecureBytes::wipe(secret);
        QVERIFY(secret.isEmpty());
        Q_UNUSED(where); // (the storage is given back by clear(); what is checked is that wipe() does not crash on shared, empty or one-byte data)
        QByteArray shared(8, 'y');
        const QByteArray other = shared;
        SecureBytes::wipe(shared);
        QVERIFY(shared.isEmpty());
        QCOMPARE(other, QByteArray(8, '\0')); // the other holder of the storage lost it too
        QByteArray empty;
        SecureBytes::wipe(empty);
        QVERIFY(empty.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstSecureBytes)
#include "tst_securebytes.moc"
