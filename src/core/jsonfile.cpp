#include "jsonfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace murasaki {
JsonFileRead readJsonObjectFile(const QString &path)
{
    JsonFileRead result;
    QFile file(path);
    if (!file.exists())
        return result;
    result.exists = true;

    // Checked before open(): opening a FIFO would block the daemon.
    if (!QFileInfo(path).isFile()) {
        result.problem = u"not a regular file"_s;
    } else if (!file.open(QIODevice::ReadOnly)) {
        result.problem = file.errorString();
    } else if (file.size() > kMaxJsonFileSize) {
        result.problem = u"file is larger than 4 MiB"_s;
    } else {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
            result.problem = parseError.errorString();
        else if (!doc.isObject())
            result.problem = u"unsupported format"_s;
        else
            result.root = doc.object();
    }
    return result;
}

bool quarantineBadFile(const QString &path, const QString &problem, const QString &fallback, const QString &noun)
{
    const QString backup = path + u".bad-"_s + QDateTime::currentDateTimeUtc().toString(u"yyyyMMddTHHmmssZ"_s);
    if (QFile::rename(path, backup)) {
        qWarning("murasakid: %s is invalid (%s); kept as %s and starting with %s", qPrintable(path),
                 qPrintable(problem), qPrintable(backup), qPrintable(fallback));
        return true;
    }
    qWarning("murasakid: %s is invalid (%s) and could not be renamed; %s will not be saved", qPrintable(path),
             qPrintable(problem), qPrintable(noun));
    return false;
}

bool writeJsonObjectFile(const QString &path, const QJsonObject &root)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning("murasakid: cannot write %s: %s", qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    file.write(QJsonDocument(root).toJson());
    if (!file.commit()) {
        qWarning("murasakid: cannot write %s: %s", qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    return true;
}

} // namespace murasaki
