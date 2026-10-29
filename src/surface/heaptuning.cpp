#include "heaptuning.h"

#include <cstdlib> // defines __GLIBC__ (nothing before an include does)

#include <sys/resource.h>

#ifdef __GLIBC__
#include <malloc.h>
#include <cstdio>
#endif

namespace murasaki {

void tuneHeap()
{
#ifdef __GLIBC__
    constexpr int kArenas = 2;
    constexpr int kBigBlock = 128 * 1024;
    const int a = mallopt(M_ARENA_MAX, kArenas);
    const int b = mallopt(M_MMAP_THRESHOLD, kBigBlock); // setting it also turns the dynamic threshold off
    const int c = mallopt(M_TRIM_THRESHOLD, kBigBlock);
    if (!a || !b || !c)
        std::fprintf(stderr, "murasaki-surface: mallopt refused a setting (arena %d, mmap %d, trim %d)\n", a, b, c);
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

unsigned long raiseDescriptorLimit()
{
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return 0;
    if (limit.rlim_cur < limit.rlim_max) {
        const rlim_t wanted = limit.rlim_max;
        limit.rlim_cur = wanted;
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0 || getrlimit(RLIMIT_NOFILE, &limit) != 0)
            return 0;
    }
    return static_cast<unsigned long>(limit.rlim_cur);
}

} // namespace murasaki
