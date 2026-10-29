#include "poolmaskloader.h"

#include "spritemask.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace murasaki {

namespace {
constexpr int kWorkerNice = 15; // behind the sprite decodes (which run at 10) and everything else
}

PoolMaskLoader::PoolMaskLoader(QString cacheRoot, QObject *parent)
    : QObject(parent)
    , m_cacheRoot(std::move(cacheRoot))
{
    m_pool.setMaxThreadCount(1);
    m_pool.setThreadPriority(QThread::LowestPriority);
}

PoolMaskLoader::~PoolMaskLoader()
{
    for (auto &job : m_jobs)
        job.cancelled->store(true);
    m_pool.waitForDone(); // (a running pass notices the flag at its next frame)
}

void PoolMaskLoader::load(const QString &key, const QString &item, const QString &variant, const QString &file, std::size_t budgetBytes, Done done)
{
    cancel(key);
    Job job;
    job.cancelled = std::make_shared<std::atomic_bool>(false);
    job.serial = ++m_serial;
    m_jobs.insert(key, job);
    const QString root = m_cacheRoot;
    QPointer<PoolMaskLoader> self(this);
    m_pool.start([=, cancelled = job.cancelled, serial = job.serial] {
        if (cancelled->load())
            return;
        setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), kWorkerNice);
        MaskLoad loaded = loadMaskPass(root, item, variant, file, cancelled.get(), budgetBytes);
        if (cancelled->load() || !self)
            return;
        std::shared_ptr<const MaskSet> masks = loaded.ok ? std::make_shared<const MaskSet>(std::move(loaded.masks)) : nullptr;
        QMetaObject::invokeMethod(self.data(), [self, key, serial, file, masks, done] {
            if (!self)
                return;
            const auto it = self->m_jobs.find(key);
            if (it == self->m_jobs.end() || it->serial != serial)
                return; // cancelled or replaced meanwhile
            self->m_jobs.erase(it);
            done(file, masks);
        }, Qt::QueuedConnection);
    });
}

void PoolMaskLoader::cancel(const QString &key)
{
    const auto it = m_jobs.find(key);
    if (it == m_jobs.end())
        return;
    it->cancelled->store(true);
    m_jobs.erase(it);
}

} // namespace murasaki
