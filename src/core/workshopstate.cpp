#include "workshopstate.h"

#include "vdf.h"

#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

bool isPlainId(const QString &id)
{
    // Workshop ids are digits; refuse anything that could leave the item dirs.
    if (id.isEmpty())
        return false;
    for (const QChar c : id) {
        if (c < u'0' || c > u'9')
            return false;
    }
    return true;
}

} // namespace

WorkshopState::WorkshopState(const QStringList &workshopRoots, const QString &appId, QObject *parent)
    : QObject(parent)
    , m_appId(appId)
{
    for (const QString &root : workshopRoots)
        m_libraries.append({root, {}});

    QStringList paths;
    for (const Library &library : std::as_const(m_libraries)) {
        // The root dir sees .acf creation/replace and downloads/ appearing, downloads/ sees
        // <appid>/ appearing, and <appid>/ sees item dirs appearing.
        paths << library.root << library.root + u"/appworkshop_"_s + m_appId + u".acf"_s
              << library.root + u"/downloads"_s << library.root + u"/downloads/"_s + m_appId;
    }
    m_watcher.setDebounceInterval(300);
    m_watcher.setPaths(paths);
    reload();
    connect(&m_watcher, &WorkshopWatcher::changed, this, [this] {
        reload();
        emit changed();
    });
}

void WorkshopState::reload()
{
    for (Library &library : m_libraries) {
        library.installed.clear();
        const QString path = library.root + u"/appworkshop_"_s + m_appId + u".acf"_s;
        if (!QFileInfo::exists(path))
            continue;
        const auto vdf = readVdfFile(path);
        if (!vdf) {
            qWarning().noquote() << "WorkshopState: cannot parse" << path << "- treating its items as not installed";
            continue;
        }
        const QVariantMap installed = vdfValue(vdfValue(*vdf, u"AppWorkshop").toMap(), u"WorkshopItemsInstalled").toMap();
        for (auto it = installed.cbegin(); it != installed.cend(); ++it)
            library.installed.insert(it.key());
    }
}

QString WorkshopState::state(const QString &workshopId) const
{
    if (!isPlainId(workshopId))
        return QString::fromLatin1(None);
    for (const Library &library : m_libraries) {
        if (QFileInfo(library.root + u"/downloads/"_s + m_appId + u'/' + workshopId).isDir())
            return QString::fromLatin1(Downloading);
    }
    for (const Library &library : m_libraries) {
        if (library.installed.contains(workshopId)
            && QFileInfo(library.root + u"/content/"_s + m_appId + u'/' + workshopId).isDir())
            return QString::fromLatin1(Installed);
    }
    return QString::fromLatin1(None);
}

} // namespace murasaki
