#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTest>
#include <QtGlobal>

// Fixture writers. Every fixture is synthetic: never copy real Workshop or Wallpaper Engine files into tests.
// A broken fixture is a bug in the test itself, so helpers abort the test binary with qFatal: a QVERIFY here
// would only return from the helper and let the test slot carry on with a half-written fixture.
namespace testutil {

inline void writeFile(const QString &path, const QByteArray &data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        qFatal("testutil: cannot create directory for %s", qPrintable(path));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size())
        qFatal("testutil: cannot write %s", qPrintable(path));
}

inline void writeProject(const QString &itemDir, const QJsonObject &project, bool withBom = false)
{
    QByteArray data = QJsonDocument(project).toJson();
    if (withBom)
        data.prepend("\xEF\xBB\xBF");
    writeFile(itemDir + "/project.json", data);
}

inline QString writeVideoItem(const QString &workshopDir, const QString &id, const QString &title)
{
    const QString dir = workshopDir + '/' + id;
    writeProject(dir, QJsonObject{{"type", "video"}, {"file", "clip.mp4"}, {"title", title}});
    writeFile(dir + "/clip.mp4", "fake video");
    return dir;
}

inline QJsonObject json(const char *text)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        qFatal("testutil: invalid fixture JSON at offset %d: %s", int(error.offset), qPrintable(error.errorString()));
    return doc.object();
}

inline QString canonical(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

} // namespace testutil
