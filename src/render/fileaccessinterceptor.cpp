#include "fileaccessinterceptor.h"

#include "pathutil.h"
#include "wallpapercontent.h"

#include <QFileInfo>
#include <QSet>

namespace murasaki {

namespace {

// Schemes that never leave the machine: the item's own files (the scheme handler confines them), inline data and
// blobs. Everything else (http, https, ws, wss, ftp, anything unknown) is the network.
bool isLocalScheme(const QString &scheme)
{
    static const QSet<QString> local{QString(kItemScheme), QStringLiteral("data"), QStringLiteral("blob"),
                                     QStringLiteral("about"), QStringLiteral("filesystem")};
    return local.contains(scheme);
}

} // namespace

void FileAccessInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    const QUrl url = info.requestUrl();
    if (url.isLocalFile()) {
        QStringList roots;
        {
            const QMutexLocker locker(&m_lock);
            roots = m_roots;
        }
        if (!isInsideAnyRoot(QFileInfo(url.toLocalFile()).canonicalFilePath(), roots))
            info.block(true);
        return;
    }
    if (isLocalScheme(url.scheme()) || m_networkAllowed)
        return;
    info.block(true);
}

} // namespace murasaki
