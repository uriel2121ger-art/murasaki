#include "importsettings.h"

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

QStringList ImportSettings::keys()
{
    return {u"importFormat"_s,    u"importSizePolicy"_s,  u"importFps"_s,         u"importKeepSource"_s,    u"importKeepAudio"_s,  u"convertGifOnImport"_s,
            u"importWorkshopFiles"_s, u"importWorkshopMdaa"_s, u"importMaxOutputMiB"_s, u"importVideoMaxSide"_s, u"importVideoMaxFps"_s, u"importRequireSandbox"_s};
}

namespace {

bool oneOf(const QJsonValue &v, std::initializer_list<const char *> allowed, QString *out)
{
    if (!v.isString())
        return false;
    for (const char *a : allowed) {
        if (v.toString() == QLatin1String(a)) {
            *out = v.toString();
            return true;
        }
    }
    return false;
}

bool whole(const QJsonValue &v, int lo, int hi, int *out)
{
    if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()) || v.toDouble() < lo || v.toDouble() > hi)
        return false;
    *out = int(v.toDouble());
    return true;
}

} // namespace

ImportSettings::Result ImportSettings::apply(const QString &key, const QJsonValue &v, QString *error)
{
    const auto bad = [&](const QString &what) {
        if (error)
            *error = key + u" "_s + what;
        return Result::Error;
    };
    if (key == u"importFormat"_s)
        return oneOf(v, {"apng", "webp"}, &format) ? Result::Ok : bad(u"must be one of apng, webp"_s);
    if (key == u"importSizePolicy"_s)
        return oneOf(v, {"fit", "scale", "first"}, &sizePolicy) ? Result::Ok : bad(u"must be one of fit, scale, first"_s);
    if (key == u"importWorkshopFiles"_s)
        return oneOf(v, {"ask", "auto"}, &workshopFiles) ? Result::Ok : bad(u"must be one of ask, auto"_s);
    if (key == u"importWorkshopMdaa"_s)
        return oneOf(v, {"ask", "auto"}, &workshopMdaa) ? Result::Ok : bad(u"must be one of ask, auto"_s);
    if (key == u"importFps"_s)
        return whole(v, 1, 120, &fps) ? Result::Ok : bad(u"must be a whole number from 1 to 120"_s);
    if (key == u"importMaxOutputMiB"_s)
        return whole(v, 16, 1024, &maxOutputMiB) ? Result::Ok : bad(u"must be a whole number from 16 to 1024"_s);
    if (key == u"importVideoMaxSide"_s)
        return whole(v, 64, 8192, &videoMaxSide) ? Result::Ok : bad(u"must be a whole number from 64 to 8192"_s);
    if (key == u"importVideoMaxFps"_s)
        return whole(v, 1, 60, &videoMaxFps) ? Result::Ok : bad(u"must be a whole number from 1 to 60"_s);
    bool *flag = key == u"importRequireSandbox"_s ? &requireSandbox : key == u"importKeepSource"_s ? &keepSource : key == u"importKeepAudio"_s ? &keepAudio : key == u"convertGifOnImport"_s ? &convertGifOnImport : nullptr;
    if (!flag)
        return Result::NotMine;
    if (!v.isBool())
        return bad(u"must be a boolean"_s);
    *flag = v.toBool();
    return Result::Ok;
}

void ImportSettings::write(QJsonObject &o) const
{
    o.insert(u"importFormat"_s, format);
    o.insert(u"importSizePolicy"_s, sizePolicy);
    o.insert(u"importFps"_s, fps);
    o.insert(u"importKeepSource"_s, keepSource);
    o.insert(u"importKeepAudio"_s, keepAudio);
    o.insert(u"convertGifOnImport"_s, convertGifOnImport);
    o.insert(u"importWorkshopFiles"_s, workshopFiles);
    o.insert(u"importWorkshopMdaa"_s, workshopMdaa);
    o.insert(u"importMaxOutputMiB"_s, maxOutputMiB);
    o.insert(u"importVideoMaxSide"_s, videoMaxSide);
    o.insert(u"importVideoMaxFps"_s, videoMaxFps);
    o.insert(u"importRequireSandbox"_s, requireSandbox);
}

} // namespace murasaki
