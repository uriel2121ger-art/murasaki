// murasaki-scene-vk heap settings: the calls must be safe to make and must give memory back after big, freed blocks.

#include "util/heap.h"

#include <QTest>

#include <cstdlib>
#include <vector>
#ifdef __GLIBC__
#include <malloc.h>
#endif

class TstSceneVkHeap : public QObject
{
    Q_OBJECT
private slots:
    void configuringTheHeapIsRepeatable()
    {
        murasaki::scene::configureHeap();
        murasaki::scene::configureHeap();
        std::vector<char> block(1 << 20, 1); // above the threshold: mapped, not taken from an arena
        QCOMPARE(block.back(), char(1));
    }

    void bigBlocksAreMappedAfterTuning()
    {
#ifdef __GLIBC__
        murasaki::scene::configureHeap();
        const size_t before = mallinfo2().hblkhd;
        void *block = std::malloc(512 * 1024); // above the threshold: mmap, which free() gives back
        QVERIFY(block);
        QVERIFY(mallinfo2().hblkhd >= before + 512 * 1024);
        std::free(block);
        QCOMPARE(mallinfo2().hblkhd, before);
#else
        QSKIP("glibc only");
#endif
    }

    void trimmingAfterManyFreedBuffersIsSafe()
    {
        // Many small buffers freed out of order leave holes in the arena; the trim has something to give back.
        std::vector<std::vector<char>> small;
        for (int i = 0; i < 20000; ++i)
            small.emplace_back(4096, char(i));
        for (size_t i = 0; i < small.size(); i += 2)
            std::vector<char>().swap(small[i]);
        small.clear();
        small.shrink_to_fit();
        const bool released = murasaki::scene::trimHeap();
#ifdef __GLIBC__
        Q_UNUSED(released) // whether pages were returned depends on the arena; the call path is what this covers
#else
        QVERIFY(!released);
#endif
    }
};

QTEST_APPLESS_MAIN(TstSceneVkHeap)
#include "tst_scenevk_heap.moc"
