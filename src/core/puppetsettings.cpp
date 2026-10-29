#include "puppetsettings.h"

#include <QFileInfo>

#include <cmath>

#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace murasaki {

QStringList PuppetSettings::keys()
{
    return {u"puppetMaxPixels"_s, u"puppetCorePath"_s};
}

PuppetSettings::Result PuppetSettings::apply(const QString &key, const QJsonValue &v, QString *error)
{
    const auto bad = [&](const QString &what) {
        if (error)
            *error = key + u" "_s + what;
        return Result::Error;
    };
    if (key == u"puppetMaxPixels"_s) {
        const double number = v.toDouble();
        if (!v.isDouble() || number != std::floor(number) || number < kMinMaxPixels || number > kMaxMaxPixels)
            return bad(u"must be a whole number from %1 to %2"_s.arg(kMinMaxPixels).arg(kMaxMaxPixels));
        maxPixels = static_cast<long long>(number);
        return Result::Ok;
    }
    if (key == u"puppetCorePath"_s) {
        if (!v.isString())
            return bad(u"must be a string"_s);
        const QString path = v.toString();
        if (path.toUtf8().size() > 4096)
            return bad(u"is too long"_s);
        for (const QChar c : path) {
            if (c.unicode() < 0x20 || c.unicode() == 0x7f)
                return bad(u"must not hold control characters"_s);
        }
        if (!path.isEmpty() && !path.startsWith(u'/'))
            return bad(u"must be empty or an absolute path"_s);
        corePath = path;
        return Result::Ok;
    }
    return Result::NotMine;
}

void PuppetSettings::write(QJsonObject &o) const
{
    o.insert(u"puppetMaxPixels"_s, maxPixels);
    o.insert(u"puppetCorePath"_s, corePath);
}

QString PuppetSettings::coreFileProblem(const QString &path)
{
    if (path.isEmpty())
        return {};
    if (!path.startsWith(u'/'))
        return u"the path is not absolute"_s;
    struct stat st {};
    // (lstat: a link is not followed - whoever can change the link can change what is loaded)
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0)
        return u"the file is not there"_s;
    if (!S_ISREG(st.st_mode))
        return u"it is not a regular file (a link, a folder or a device)"_s;
    if (st.st_uid != ::geteuid() && st.st_uid != 0)
        return u"it belongs to someone who is neither you nor root"_s;
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        return u"it can be written by others than its owner"_s;
    return {};
}

} // namespace murasaki
