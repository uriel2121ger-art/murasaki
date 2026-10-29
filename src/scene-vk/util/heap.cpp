#include "util/heap.h"

#include <cstdlib> // defines __GLIBC__ (nothing before an include does)

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace murasaki::scene {

void configureHeap()
{
#ifdef __GLIBC__
    if (std::getenv("MURASAKI_SCENE_VK_HEAP_DEFAULT")) {
        return;
    }
    constexpr int kMmapThreshold = 256 * 1024;
    // Setting the mmap threshold turns the dynamic adjustment off, so the trim threshold is fixed with it.
    mallopt(M_MMAP_THRESHOLD, kMmapThreshold);
    mallopt(M_TRIM_THRESHOLD, kMmapThreshold);
#endif
}

bool trimHeap()
{
#ifdef __GLIBC__
    return malloc_trim(0) != 0;
#else
    return false;
#endif
}

} // namespace murasaki::scene
