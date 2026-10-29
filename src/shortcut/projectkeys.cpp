#include "shortcut/projectkeys.h"

#include "shortcut/shortcutpolicy.h"

#include <QFile>
#include <QJsonDocument>

#include <algorithm>

namespace murasaki::shortcut {

QStringList userShortcutKeys(const QJsonObject &properties)
{
    QStringList keys;
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!it.value().isObject() || it.value().toObject().value(QStringLiteral("type")).toString().toLower() != QLatin1String("usershortcut")) {
            continue;
        }
        if (!validKey(it.key().toUtf8())) {
            continue;
        }
        keys << it.key();
        if (keys.size() >= kMaxSlots) {
            break;
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

QStringList userShortcutKeys(const QString &itemDir)
{
    QFile file(itemDir + QStringLiteral("/project.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxProjectBytes) {
        return {};
    }
    QByteArray data = file.read(kMaxProjectBytes + 1);
    if (data.startsWith("\xEF\xBB\xBF")) {
        data.remove(0, 3); // a byte order mark
    }
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    return userShortcutKeys(doc.object().value(QStringLiteral("general")).toObject().value(QStringLiteral("properties")).toObject());
}

} // namespace murasaki::shortcut
