#pragma once

#include "maskloader.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QThreadPool>

#include <atomic>
#include <memory>

namespace murasaki {

// The production MaskLoader: one low-priority worker thread (so the passes of all the pets run one at a time, behind everything else), the
// disk cache under `cacheRoot` (loadMaskPass), the answer posted back to the thread that created the loader.
class PoolMaskLoader : public QObject, public MaskLoader
{
    Q_OBJECT

public:
    explicit PoolMaskLoader(QString cacheRoot, QObject *parent = nullptr);
    ~PoolMaskLoader() override;

    void load(const QString &key, const QString &item, const QString &variant, const QString &file, std::size_t budgetBytes, Done done) override;
    void cancel(const QString &key) override;

private:
    struct Job {
        std::shared_ptr<std::atomic_bool> cancelled;
        quint64 serial = 0;
    };
    QString m_cacheRoot;
    QThreadPool m_pool;
    QHash<QString, Job> m_jobs;
    quint64 m_serial = 0;
};

} // namespace murasaki
