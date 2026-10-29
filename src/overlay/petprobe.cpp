// murasaki-petprobe --stage LAYOUT.json prints the slices of a stage for a layout (tools/compat/stage_geometry_check.py compares them with an independent implementation).
// murasaki-petprobe FILE OUTDIR: the pets sweep's decode (tools/compat/pets_sweep.py). Reads an animation the way the overlay does
// (SpriteDecoder, FFmpeg or Qt's WebP plugin) but as a stream, keeps its first, middle and last frames and writes them as PNG into OUTDIR,
// and prints one JSON line: what the header promises, what the decoder produced, the span of the frames' times and where the pictures are.
// Meant for item files, which are untrusted: the file is opened only through the decoder's local-file path, nothing else is read.

#include "animationinfo.h"
#include "spritedecoder.h"
#include "stagegeometry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

using namespace murasaki;
using namespace Qt::StringLiterals;

// `--stage` reads {"outputs": [{name, x, y, width, height, scale}], "mode": "output"|"desktop", "output": name, "footprint": [w, h], "fit": ...} from a file and prints
// {"picture": [x, y, w, h], "slices": [{output, global, window, source}], "degraded": [...]}.
static int stageMain(const QString &path)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly) || in.size() > (1 << 20)) {
        std::fprintf(stderr, "murasaki-petprobe: cannot read the layout\n");
        return 2;
    }
    const QJsonObject o = QJsonDocument::fromJson(in.readAll()).object();
    QList<StageOutput> outputs;
    for (const QJsonValue &value : o.value(u"outputs"_s).toArray()) {
        if (outputs.size() >= 64)
            break; // (a layout is a handful of outputs; the probe is not a way to make the geometry work on thousands)
        const QJsonObject e = value.toObject();
        StageOutput out;
        out.name = e.value(u"name"_s).toString();
        out.rect = QRect(e.value(u"x"_s).toInt(), e.value(u"y"_s).toInt(), e.value(u"width"_s).toInt(), e.value(u"height"_s).toInt());
        out.scale = e.value(u"scale"_s).toDouble(1);
        outputs.append(out);
    }
    const auto mode = stageModeFromName(o.value(u"mode"_s).toString());
    const auto fit = stageFitFromName(o.value(u"fit"_s).toString());
    const QJsonArray footprint = o.value(u"footprint"_s).toArray();
    if (!mode || !fit || footprint.size() != 2) {
        std::fprintf(stderr, "murasaki-petprobe: the layout needs mode, fit and footprint\n");
        return 2;
    }
    const StageLayout layout = stageLayout(outputs, *mode, o.value(u"output"_s).toString(), QSizeF(footprint.at(0).toDouble(), footprint.at(1).toDouble()), *fit);
    const auto rect = [](const QRectF &r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
    QJsonArray slices;
    for (const StageSlice &slice : layout.slices) {
        slices.append(QJsonObject{{u"output"_s, slice.output}, {u"global"_s, rect(QRectF(slice.global))}, {u"window"_s, rect(QRectF(slice.window))}, {u"source"_s, rect(slice.source)}});
    }
    const QJsonObject result{{u"picture"_s, rect(layout.picture)}, {u"slices"_s, slices}, {u"degraded"_s, QJsonArray::fromStringList(layout.degraded)}};
    std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 3 && QByteArrayView(argv[1]) == "--stage")
        return stageMain(QString::fromLocal8Bit(argv[2]));
    if (argc != 3) {
        std::fprintf(stderr, "usage: murasaki-petprobe FILE OUTDIR | --stage LAYOUT.json\n");
        return 2;
    }
    const QString file = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString outDir = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(outDir);
    QString problem;
    const qint64 promised = SpriteDecoder::promisedFrames(file, &problem);
    QList<qint64> indexes{0};
    // The file's own chunk walk says how many frames there are, at the cost of reading the chunk headers and no pixels, so the decode below is
    // the only pass over the data. It comes first: FFmpeg's count before decoding is an estimate (an APNG's demuxer gives none, and a GIF with a
    // second stream appended is counted up to the second header only).
    qint64 known = 0;
    if (const auto info = readAnimationInfo(file))
        known = info->frames;
    if (known == 0)
        known = promised;
    if (known == 0) {
        // a container neither knows (rare): one pass over it to count the frames
        const SpriteDecoder::Probe counted = SpriteDecoder::probe(file, indexes);
        known = counted.ok ? counted.frames : 0;
    }
    if (known > 2)
        indexes.append(known / 2);
    const SpriteDecoder::Probe probe = SpriteDecoder::probe(file, indexes);
    QJsonObject out{{"file", file}, {"ok", probe.ok}, {"promised", double(promised)}};
    if (!probe.ok) {
        out["error"] = probe.error.isEmpty() ? problem : probe.error;
    } else {
        out["frames"] = double(probe.frames);
        out["duration_ms"] = probe.durationMs;
        out["width"] = probe.size.width();
        out["height"] = probe.size.height();
        out["resized_frames"] = double(probe.resizedFrames); // frames of another size than the stream's, scaled to it
        if (probe.resizedFrames > 0)
            out["other_size"] = QStringLiteral("%1x%2").arg(probe.otherSize.width()).arg(probe.otherSize.height());
        QJsonObject picked;
        for (auto it = probe.images.constBegin(); it != probe.images.constEnd(); ++it) {
            const char *role = it.key() == 0 ? "first" : it.key() == probe.frames - 1 ? "last" : "middle";
            const QString path = QDir(outDir).filePath(QString::fromLatin1(role) + QStringLiteral(".png"));
            // straight pixels in the file: what an independent reader gives
            const bool saved = it.value().convertedTo(QImage::Format_RGBA8888).save(path, "PNG");
            picked[QString::fromLatin1(role)] = QJsonObject{{"index", double(it.key())}, {"file", saved ? path : QString()}};
        }
        out["picked"] = picked;
    }
    std::fputs(QJsonDocument(out).toJson(QJsonDocument::Compact).constData(), stdout);
    std::fputc('\n', stdout);
    return probe.ok ? 0 : 1;
}
