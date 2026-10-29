#pragma once

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>

#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace murasaki {

// Helpers shared by the files that define MurasakiService (murasakiservice*.cpp).
namespace murasakiservice_internal {

// The import record of an item, read only when it is a plain file of a sane size: the folder is the user's data, and a FIFO or a device named source.json must not
// make the daemon wait or read without end.
inline QJsonObject readRecordFile(const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return {};
    struct stat st{};
    QByteArray bytes;
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= (8 << 20)) {
        QFile file;
        if (file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
            return QJsonDocument::fromJson(file.readAll()).object();
    }
    ::close(fd);
    return {};
}

inline std::optional<QJsonObject> parseObject(const QString &json, bool emptyIsObject)
{
    if (json.trimmed().isEmpty())
        return emptyIsObject ? std::optional<QJsonObject>(QJsonObject()) : std::nullopt;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return doc.object();
}

} // namespace murasakiservice_internal

using namespace murasakiservice_internal;

} // namespace murasaki
