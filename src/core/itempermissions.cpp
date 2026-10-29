#include "itempermissions.h"

#include "jsonfile.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

bool validItemId(const QString &itemId)
{
    return !itemId.isEmpty() && itemId.size() <= 200;
}

} // namespace

ItemPermissionStore::ItemPermissionStore(QString filePath, QObject *parent)
    : QObject(parent)
    , m_path(std::move(filePath))
{
}

void ItemPermissionStore::load()
{
    m_network.clear();
    m_saveBlocked = false;
    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists)
        return;

    QString problem = read.problem;
    if (problem.isEmpty()) {
        const QJsonValue items = read.root.value(u"items"_s);
        if (read.root.value(u"version"_s).toInt() != 1 || !items.isObject()) {
            problem = u"unsupported format"_s;
        } else {
            const QJsonObject object = items.toObject();
            if (object.size() > kMaxPermissionGrants)
                problem = u"too many entries"_s;
            for (auto it = object.begin(); problem.isEmpty() && it != object.end(); ++it) {
                const QJsonValue network = it.value().toObject().value(u"network"_s);
                if (!validItemId(it.key()) || !it.value().isObject() || !(network.isBool() || network.isUndefined()))
                    problem = u"invalid entry: "_s + it.key().left(200);
                else if (network.toBool())
                    m_network.insert(it.key());
            }
        }
    }
    if (problem.isEmpty())
        return;

    m_network.clear();
    if (!quarantineBadFile(m_path, problem, u"no permissions granted"_s, u"permissions"_s))
        m_saveBlocked = true;
}

bool ItemPermissionStore::setNetwork(const QString &itemId, bool allowed, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (error)
        error->clear();
    if (!validItemId(itemId))
        return fail(u"itemId must be 1-200 characters"_s);
    if (m_network.contains(itemId) == allowed)
        return true;
    if (allowed && m_network.size() >= kMaxPermissionGrants)
        return fail(u"too many permission grants"_s);
    const QSet<QString> previous = m_network;
    if (allowed)
        m_network.insert(itemId);
    else
        m_network.remove(itemId);
    if (!save()) {
        m_network = previous;
        return fail(u"could not save the permissions"_s);
    }
    emit changed(itemId);
    return true;
}

bool ItemPermissionStore::save() const
{
    if (m_saveBlocked) {
        qWarning("murasakid: not saving permissions over the unreadable %s", qPrintable(m_path));
        return false;
    }
    QStringList ids(m_network.begin(), m_network.end());
    std::sort(ids.begin(), ids.end());
    QJsonObject items;
    for (const QString &id : std::as_const(ids))
        items.insert(id, QJsonObject{{u"network"_s, true}});
    return writeJsonObjectFile(m_path, QJsonObject{{u"version"_s, 1}, {u"items"_s, items}});
}

} // namespace murasaki
