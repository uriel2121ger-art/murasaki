#include "securebytes.h"

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

namespace murasaki {
namespace {

std::atomic<int> g_live{0};
std::atomic<qint64> g_bytes{0};
QMutex g_observerLock;
std::function<void(const char *, qsizetype, bool)> g_observer;

qsizetype pageSize()
{
    static const qsizetype size = qsizetype(sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096);
    return size;
}

// memset that the optimiser may not remove.
void zero(void *data, size_t size)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
    for (size_t i = 0; i < size; ++i)
        p[i] = 0;
}

} // namespace

SecureBytes::SecureBytes(QByteArrayView data)
{
    if (data.isEmpty())
        return;
    const qsizetype page = pageSize();
    const qsizetype mapped = (data.size() + page - 1) / page * page;
    void *memory = mmap(nullptr, size_t(mapped), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return;
#ifdef MADV_DONTDUMP
    if (madvise(memory, size_t(mapped), MADV_DONTDUMP) != 0) {
        munmap(memory, size_t(mapped)); // not excluded from a dump: not kept at all
        return;
    }
#endif
#ifdef MADV_WIPEONFORK
    madvise(memory, size_t(mapped), MADV_WIPEONFORK); // (a child of fork() sees zeroes; a refusal is harmless)
#endif
    std::memcpy(memory, data.constData(), size_t(data.size()));
    m_data = static_cast<char *>(memory);
    m_size = data.size();
    m_mapped = mapped;
    ++g_live;
    g_bytes += m_mapped;
}

SecureBytes &SecureBytes::operator=(SecureBytes &&other) noexcept
{
    if (this != &other) {
        clear();
        m_data = other.m_data;
        m_size = other.m_size;
        m_mapped = other.m_mapped;
        other.m_data = nullptr;
        other.m_size = other.m_mapped = 0;
    }
    return *this;
}

void SecureBytes::clear()
{
    if (!m_data)
        return;
    zero(m_data, size_t(m_mapped));
    {
        QMutexLocker lock(&g_observerLock);
        if (g_observer) {
            const bool allZero = std::all_of(m_data, m_data + m_mapped, [](char c) { return c == 0; });
            g_observer(m_data, m_mapped, allZero);
        }
    }
    munmap(m_data, size_t(m_mapped));
    --g_live;
    g_bytes -= m_mapped;
    m_data = nullptr;
    m_size = m_mapped = 0;
}

void SecureBytes::wipe(QByteArray &bytes)
{
    // In place, shared or not: the caller owns the secret, so every holder of this storage loses it too (data() would first copy a shared one and leave the original).
    if (!bytes.isEmpty())
        zero(const_cast<char *>(bytes.constData()), size_t(bytes.size()));
    bytes.clear();
}

int SecureBytes::liveBuffers()
{
    return g_live.load();
}

qint64 SecureBytes::liveBytes()
{
    return g_bytes.load();
}

void SecureBytes::setWipeObserver(std::function<void(const char *, qsizetype, bool)> observer)
{
    QMutexLocker lock(&g_observerLock);
    g_observer = std::move(observer);
}

} // namespace murasaki
