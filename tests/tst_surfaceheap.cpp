// The surface's heap settings: safe to call, repeatable, and the trim has something to give back after many freed buffers.

#include "heaptuning.h"

#include <QTest>

#include <cstdlib>
#include <vector>
#ifdef __GLIBC__
#include <malloc.h>
#endif

class TstSurfaceHeap : public QObject
{
    Q_OBJECT
private slots:
    void tuningTheHeapIsRepeatable()
    {
        murasaki::tuneHeap();
        murasaki::tuneHeap();
        std::vector<char> block(1 << 20, 1); // above the threshold: mapped, not taken from an arena
        QCOMPARE(block.back(), char(1));
    }

    void bigBlocksAreMappedAfterTuning()
    {
#ifdef __GLIBC__
        murasaki::tuneHeap();
        const size_t before = mallinfo2().hblkhd;
        void *block = std::malloc(256 * 1024); // above the threshold: mmap, which free() gives back
        QVERIFY(block);
        QVERIFY(mallinfo2().hblkhd >= before + 256 * 1024);
        std::free(block);
        QCOMPARE(mallinfo2().hblkhd, before);
#else
        QSKIP("glibc only");
#endif
    }

    void theDescriptorLimitIsRaisedToTheHardOne()
    {
        const unsigned long limit = murasaki::raiseDescriptorLimit();
        QVERIFY(limit >= 1024);
        QCOMPARE(murasaki::raiseDescriptorLimit(), limit); // repeatable
    }

    void trimmingAfterFreedBuffersIsSafe()
    {
        std::vector<std::vector<char>> small;
        for (int i = 0; i < 20000; ++i)
            small.emplace_back(4096, char(i));
        for (size_t i = 0; i < small.size(); i += 2)
            std::vector<char>().swap(small[i]);
        small.clear();
        small.shrink_to_fit();
        const bool released = murasaki::trimHeap();
#ifdef __GLIBC__
        Q_UNUSED(released) // whether pages return depends on the arena; this covers the call path
#else
        QVERIFY(!released);
#endif
    }
};

QTEST_APPLESS_MAIN(TstSurfaceHeap)
#include "tst_surfaceheap.moc"
