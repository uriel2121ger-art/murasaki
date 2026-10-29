#include "importspec.h"

#include "petimport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

namespace murasaki::import {

namespace {

QString refused(const QString &detail)
{
    return QString::fromUtf8(kSpecRefused.data(), qsizetype(kSpecRefused.size())) + QStringLiteral(": ") + detail;
}

// An integer member: absent leaves *out; present must be a whole number within [lo, hi].
bool readInt(const QJsonObject &o, const char *key, int lo, int hi, int *out, QString *error)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined())
        return true;
    if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()) || v.toDouble() < lo || v.toDouble() > hi) {
        *error = refused(QStringLiteral("%1 must be a whole number from %2 to %3").arg(QLatin1String(key)).arg(lo).arg(hi));
        return false;
    }
    *out = v.toInt();
    return true;
}

} // namespace

SpecResult parseImportSpec(const QByteArray &json)
{
    SpecResult result;
    if (json.size() > kMaxSpecBytes) {
        result.error = refused(QStringLiteral("over %1 bytes").arg(kMaxSpecBytes));
        return result;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = refused(QStringLiteral("not a JSON object"));
        return result;
    }
    const QJsonObject o = doc.object();
    ImportSpec spec;
    const QString kind = o.value(QLatin1String("kind")).toString();
    if (kind == QLatin1String("sheet"))
        spec.kind = ImportKind::Sheet;
    else if (kind == QLatin1String("images"))
        spec.kind = ImportKind::Images;
    else if (kind == QLatin1String("zip"))
        spec.kind = ImportKind::Zip;
    else if (kind == QLatin1String("gif"))
        spec.kind = ImportKind::Gif;
    else if (kind == QLatin1String("mdaa"))
        spec.kind = ImportKind::Mdaa;
    else if (kind == QLatin1String("video"))
        spec.kind = ImportKind::Video;
    else {
        result.error = refused(QStringLiteral("kind must be sheet, images, zip, gif, mdaa or video"));
        return result;
    }

    if (const QJsonValue n = o.value(QLatin1String("name")); !n.isUndefined()) {
        if (!n.isString() || n.toString().size() > 200) {
            result.error = refused(QStringLiteral("name must be text of at most 200 characters"));
            return result;
        }
        spec.name = n.toString();
    }
    const QJsonValue sources = o.value(QLatin1String("sources"));
    if (!sources.isArray() || sources.toArray().isEmpty() || sources.toArray().size() > kMaxSources) {
        result.error = refused(QStringLiteral("sources must be a list of 1 to %1 paths").arg(kMaxSources));
        return result;
    }
    for (const QJsonValue &s : sources.toArray()) {
        const QString path = s.toString();
        if (!s.isString() || path.isEmpty() || !path.startsWith(QLatin1Char('/')) || path.contains(QChar(0)) || path.toUtf8().size() > 4096) {
            result.error = refused(QStringLiteral("every source must be an absolute path"));
            return result;
        }
        spec.sources.append(path);
    }
    const bool single = spec.kind != ImportKind::Images;
    if (single && spec.sources.size() != 1) {
        result.error = refused(QStringLiteral("this kind takes exactly one source"));
        return result;
    }

    QString error;
    SheetSpec &sh = spec.sheet;
    if (!readInt(o, "frameWidth", 1, kMaxCanvasSide, &sh.frameWidth, &error) || !readInt(o, "frameHeight", 1, kMaxCanvasSide, &sh.frameHeight, &error)
        || !readInt(o, "frameCount", 1, kMaxFrames, &sh.frameCount, &error) || !readInt(o, "startFrame", 1, kMaxFrames * 4, &sh.startFrame, &error)
        || !readInt(o, "paddingLeft", 0, kMaxSheetSide, &sh.paddingLeft, &error) || !readInt(o, "paddingTop", 0, kMaxSheetSide, &sh.paddingTop, &error)
        || !readInt(o, "spacingH", 0, kMaxSheetSide, &sh.spacingHorizontal, &error) || !readInt(o, "spacingV", 0, kMaxSheetSide, &sh.spacingVertical, &error)
        || !readInt(o, "columns", 0, kMaxSheetSide, &sh.columns, &error) || !readInt(o, "rows", 0, kMaxSheetSide, &sh.rows, &error)
        || !readInt(o, "frameMs", 1, kMaxFrameMs, &spec.frameMs, &error)) {
        result.error = error;
        return result;
    }
    if (const QJsonValue f = o.value(QLatin1String("fps")); !f.isUndefined()) {
        if (!f.isDouble() || !(f.toDouble() >= 1 && f.toDouble() <= 120)) {
            result.error = refused(QStringLiteral("fps must be a number from 1 to 120"));
            return result;
        }
        spec.fps = f.toDouble();
    }
    if (const QJsonValue p = o.value(QLatin1String("sizePolicy")); !p.isUndefined()) {
        const QString text = p.toString();
        if (text == QLatin1String("first"))
            spec.sizePolicy = SizePolicy::First;
        else if (text == QLatin1String("fit"))
            spec.sizePolicy = SizePolicy::Fit;
        else if (text == QLatin1String("scale"))
            spec.sizePolicy = SizePolicy::Scale;
        else {
            result.error = refused(QStringLiteral("sizePolicy must be first, fit or scale"));
            return result;
        }
    }
    if (const QJsonValue f = o.value(QLatin1String("format")); !f.isUndefined()) {
        if (f.toString() == QLatin1String("apng"))
            spec.format = OutputFormat::Apng;
        else if (f.toString() == QLatin1String("webp"))
            spec.format = OutputFormat::Webp;
        else {
            result.error = refused(QStringLiteral("format must be apng or webp"));
            return result;
        }
    }
    for (const char *key : {"startMs", "durationMs"}) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isUndefined() && (!v.isDouble() || v.toDouble() != std::floor(v.toDouble()) || v.toDouble() < 0 || v.toDouble() > 24.0 * 3600 * 1000)) {
            result.error = refused(QStringLiteral("%1 must be a whole number of milliseconds from 0 to 24 hours").arg(QLatin1String(key)));
            return result;
        }
    }
    spec.startMs = qint64(o.value(QLatin1String("startMs")).toDouble(0));
    spec.durationMs = qint64(o.value(QLatin1String("durationMs")).toDouble(0));
    if (const QJsonValue v = o.value(QLatin1String("maxFps")); !v.isUndefined()) {
        if (!v.isDouble() || !(v.toDouble() >= 1 && v.toDouble() <= 60)) {
            result.error = refused(QStringLiteral("maxFps must be a number from 1 to 60"));
            return result;
        }
        spec.maxFps = v.toDouble();
    }
    if (!readInt(o, "maxSide", 16, 8192, &spec.maxSide, &error) ||
        !readInt(o, "webpQuality", 1, 100, &spec.webpQuality, &error) || !readInt(o, "autoScene", -1, 255, &spec.autoScene, &error)) {
        result.error = error;
        return result;
    }
    for (const char *key : {"keepSource", "keepAudio"}) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isUndefined() && !v.isBool()) {
            result.error = refused(QStringLiteral("%1 must be true or false").arg(QLatin1String(key)));
            return result;
        }
    }
    spec.keepSource = o.value(QLatin1String("keepSource")).toBool(false);
    spec.keepAudio = o.value(QLatin1String("keepAudio")).toBool(false);
    if (const QJsonValue k = o.value(QLatin1String("colorKey")); !k.isUndefined()) {
        const QJsonObject ko = k.toObject();
        const QString hex = ko.value(QLatin1String("color")).toString();
        bool ok = k.isObject() && hex.size() == 6;
        const uint rgb = ok ? hex.toUInt(&ok, 16) : 0;
        const QJsonValue tol = ko.value(QLatin1String("tolerance")), soft = ko.value(QLatin1String("softness"));
        const bool tolOk = tol.isUndefined() || (tol.isDouble() && tol.toDouble() >= 0 && tol.toDouble() <= 100);
        const bool softOk = soft.isUndefined() || (soft.isDouble() && soft.toDouble() >= 0 && soft.toDouble() <= 100);
        if (!ok || !tolOk || !softOk) {
            result.error = refused(QStringLiteral("colorKey needs color RRGGBB, tolerance 0..100 and softness 0..100"));
            return result;
        }
        spec.colorKey.enabled = true;
        spec.colorKey.red = quint8(rgb >> 16);
        spec.colorKey.green = quint8(rgb >> 8);
        spec.colorKey.blue = quint8(rgb);
        spec.colorKey.tolerance = tol.isUndefined() ? 0.5 : tol.toDouble() / 100.0;
        spec.colorKey.softness = soft.isUndefined() ? 0.1 : soft.toDouble() / 100.0;
    }
    // Options that belong to one kind are refused for the others, so a mistake is told and not silently ignored (and cannot change an import's id).
    const bool isVideo = spec.kind == ImportKind::Video;
    for (const char *key : {"maxSide", "maxFps", "startMs", "durationMs", "keepAudio"}) {
        if (!isVideo && o.contains(QLatin1String(key))) {
            result.error = refused(QStringLiteral("%1 only applies to a video").arg(QLatin1String(key)));
            return result;
        }
    }
    if ((spec.kind == ImportKind::Video || spec.kind == ImportKind::Gif || spec.kind == ImportKind::Mdaa) && (o.contains(QLatin1String("fps")) || o.contains(QLatin1String("frameMs")))) {
        result.error = refused(QStringLiteral("fps and frameMs do not apply to this kind (it has its own timing)"));
        return result;
    }
    if (spec.frameMs != 0 && spec.fps != 0) {
        result.error = refused(QStringLiteral("give frameMs or fps, not both"));
        return result;
    }
    sh.frameMs = spec.kind == ImportKind::Sheet && spec.frameMs ? spec.frameMs : sh.frameMs;
    result.spec = spec;
    return result;
}

QByteArray canonicalJson(const ImportSpec &spec)
{
    static const char *const kinds[] = {"sheet", "images", "zip", "gif", "mdaa", "video"};
    static const char *const policies[] = {"first", "fit", "scale"};
    QJsonObject o; // QJsonObject keeps its keys sorted
    o.insert(QStringLiteral("kind"), QLatin1String(kinds[int(spec.kind)]));
    o.insert(QStringLiteral("name"), spec.name);
    if (spec.kind == ImportKind::Sheet || spec.kind == ImportKind::Images || spec.kind == ImportKind::Zip)
        o.insert(QStringLiteral("frameMs"), effectiveFrameMs(spec)); // (a GIF, a video and a package keep their own timing)
    if (spec.kind == ImportKind::Sheet) {
        const SheetSpec &s = spec.sheet;
        o.insert(QStringLiteral("frameWidth"), s.frameWidth);
        o.insert(QStringLiteral("frameHeight"), s.frameHeight);
        o.insert(QStringLiteral("frameCount"), s.frameCount);
        o.insert(QStringLiteral("startFrame"), s.startFrame);
        o.insert(QStringLiteral("paddingLeft"), s.paddingLeft);
        o.insert(QStringLiteral("paddingTop"), s.paddingTop);
        o.insert(QStringLiteral("spacingH"), s.spacingHorizontal);
        o.insert(QStringLiteral("spacingV"), s.spacingVertical);
        o.insert(QStringLiteral("columns"), s.columns);
        o.insert(QStringLiteral("rows"), s.rows);
    }
    if (spec.kind == ImportKind::Video) {
        o.insert(QStringLiteral("maxSide"), spec.maxSide);
        o.insert(QStringLiteral("maxFps"), spec.maxFps);
        o.insert(QStringLiteral("startMs"), double(spec.startMs));
        o.insert(QStringLiteral("durationMs"), double(spec.durationMs));
    }
    if (spec.kind == ImportKind::Images || spec.kind == ImportKind::Zip)
        o.insert(QStringLiteral("sizePolicy"), QLatin1String(policies[int(spec.sizePolicy)]));
    o.insert(QStringLiteral("format"), spec.format == OutputFormat::Webp ? QLatin1String("webp") : QLatin1String("apng"));
    if (spec.format == OutputFormat::Webp)
        o.insert(QStringLiteral("webpQuality"), spec.webpQuality);
    o.insert(QStringLiteral("keepSource"), spec.keepSource);
    o.insert(QStringLiteral("keepAudio"), spec.keepAudio);
    // (autoScene is not here: which scene a pet starts in is the pet's, kept in the record; the same package with another choice is the same folder)
    if (spec.colorKey.enabled)
        o.insert(QStringLiteral("colorKey"), QJsonObject{{QStringLiteral("color"), QStringLiteral("%1").arg(quint32(spec.colorKey.red) << 16 | quint32(spec.colorKey.green) << 8 | spec.colorKey.blue, 6, 16, QLatin1Char('0'))},
                                                         {QStringLiteral("tolerance"), spec.colorKey.tolerance * 100.0},
                                                         {QStringLiteral("softness"), spec.colorKey.softness * 100.0}});
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

int effectiveFrameMs(const ImportSpec &spec)
{
    if (spec.frameMs > 0)
        return spec.frameMs;
    if (spec.fps > 0)
        return qBound(1, int(std::lround(1000.0 / spec.fps)), kMaxFrameMs);
    if (spec.kind == ImportKind::Sheet)
        return 100;
    return qBound(1, int(std::lround(1000.0 / kDefaultPetFps)), kMaxFrameMs); // gif keeps its own delays and ignores this
}

QString sanitiseOutputName(const QString &name)
{
    QString out;
    QByteArray bytes;
    for (const QChar c : name.trimmed()) {
        const bool keep = c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.') || c == QLatin1Char(' ');
        const QString piece = keep ? QString(c) : QStringLiteral("_");
        if (bytes.size() + piece.toUtf8().size() > 64)
            break;
        bytes += piece.toUtf8();
        out += piece;
    }
    while (out.contains(QStringLiteral("..")))
        out.replace(QStringLiteral(".."), QStringLiteral("._"));
    while (out.startsWith(QLatin1Char('.')) || out.startsWith(QLatin1Char(' ')))
        out.remove(0, 1);
    while (out.endsWith(QLatin1Char(' ')) || out.endsWith(QLatin1Char('.')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("pet") : out;
}

bool naturalLess(const QString &a, const QString &b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a.at(i).isDigit() && b.at(j).isDigit()) {
            qsizetype ie = i, je = j;
            while (ie < a.size() && a.at(ie).isDigit())
                ++ie;
            while (je < b.size() && b.at(je).isDigit())
                ++je;
            qsizetype is = i, js = j;
            while (is < ie - 1 && a.at(is) == QLatin1Char('0'))
                ++is;
            while (js < je - 1 && b.at(js) == QLatin1Char('0'))
                ++js;
            const qsizetype lenA = ie - is, lenB = je - js;
            if (lenA != lenB)
                return lenA < lenB;
            const int cmp = QStringView(a).mid(is, lenA).compare(QStringView(b).mid(js, lenB));
            if (cmp != 0)
                return cmp < 0;
            i = ie;
            j = je;
        } else {
            const QChar ca = a.at(i).toCaseFolded(), cb = b.at(j).toCaseFolded();
            if (ca != cb)
                return ca < cb;
            ++i;
            ++j;
        }
    }
    if (i - a.size() != j - b.size())
        return (a.size() - i) < (b.size() - j);
    return a < b;
}

} // namespace murasaki::import
