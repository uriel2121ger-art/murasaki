// murasaki-petimport: runs one pet import (importjob.h) and reports it as JSON lines on stdout.
//
//   murasaki-petimport --spec FILE --out-root DIR [--max-output BYTES] [--preview]
//
// The daemon starts it behind murasaki-sandbox (read the sources, write DIR, no network). It limits itself as well: RLIMIT_AS 4 GiB, RLIMIT_CPU 60
// minutes, nice 19. SIGTERM or SIGINT cancels between two frames and removes the work folder.
// stdout, one JSON object per line: {"progress":0.4,"frames":120} while it works, then exactly one of
//   {"done":true,"id":...,"dir":...,"file":...,"frames":...,"width":...,"height":...,"totalDelayMs":...,"bytes":...,"reused":...,"degraded":[...]}
//   {"error":"<name: detail>"}
// --preview reads headers only and prints {"preview":{...}} or {"error":...}. Exit status: 0 done, 1 refused or failed, 2 usage.

#include "importjob.h"
#include "importspec.h"
#include "petimport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>

namespace {

std::atomic<bool> gCancel{false};

void onSignal(int)
{
    gCancel.store(true);
}

void line(const QJsonObject &o)
{
    const QByteArray bytes = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

int refuse(const QString &message)
{
    line({{QStringLiteral("error"), message}});
    return 1;
}

void limitSelf()
{
    const rlimit as{4ULL << 30, 4ULL << 30};
    ::setrlimit(RLIMIT_AS, &as);
    const rlimit cpu{60 * 60, 60 * 60}; // CPU time adds up over the decoder threads; the wall-clock budget (15 minutes) ends a job long before
    ::setrlimit(RLIMIT_CPU, &cpu);
    const rlimit core{0, 0};
    ::setrlimit(RLIMIT_CORE, &core);
    ::setpriority(PRIO_PROCESS, 0, 19);
}

} // namespace

int main(int argc, char **argv)
{
    limitSelf();
    QCoreApplication app(argc, argv);
    QString specPath, outRoot;
    long long maxOutput = murasaki::import::kMaxOutputBytes;
    bool preview = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("--spec") && i + 1 < args.size()) {
            specPath = args.at(++i);
        } else if (a == QLatin1String("--out-root") && i + 1 < args.size()) {
            outRoot = args.at(++i);
        } else if (a == QLatin1String("--max-output") && i + 1 < args.size()) {
            bool ok = false;
            maxOutput = args.at(++i).toLongLong(&ok);
            if (!ok || maxOutput < 1 || maxOutput > murasaki::import::kMaxOutputBytes) {
                std::fprintf(stderr, "murasaki-petimport: --max-output must be 1..%lld\n", murasaki::import::kMaxOutputBytes);
                return 2;
            }
        } else if (a == QLatin1String("--preview")) {
            preview = true;
        } else {
            std::fprintf(stderr, "usage: murasaki-petimport --spec FILE --out-root DIR [--max-output BYTES] [--preview]\n");
            return 2;
        }
    }
    if (specPath.isEmpty() || (outRoot.isEmpty() && !preview)) {
        std::fprintf(stderr, "usage: murasaki-petimport --spec FILE --out-root DIR [--max-output BYTES] [--preview]\n");
        return 2;
    }

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    QFile specFile(specPath);
    if (!specFile.open(QIODevice::ReadOnly))
        return refuse(QString::fromUtf8(murasaki::import::kSpecRefused.data()) + QStringLiteral(": the spec file cannot be read"));
    const QByteArray json = specFile.read(murasaki::import::kMaxSpecBytes + 1);
    const murasaki::import::SpecResult parsed = murasaki::import::parseImportSpec(json);
    if (!parsed.ok())
        return refuse(parsed.error);

    if (preview) {
        const murasaki::import::ImportPreview p = murasaki::import::previewImport(*parsed.spec);
        if (!p.ok)
            return refuse(p.error);
        line({{QStringLiteral("preview"),
               QJsonObject{{QStringLiteral("frames"), p.frames},
                           {QStringLiteral("width"), p.width},
                           {QStringLiteral("height"), p.height},
                           {QStringLiteral("totalDelayMs"), double(p.totalDelayMs)},
                           {QStringLiteral("estimatedBytes"), double(p.estimatedBytes)},
                           {QStringLiteral("degraded"), QJsonArray::fromStringList(p.degraded)}}}});
        return 0;
    }

    murasaki::import::ImportOptions options;
    options.outRoot = outRoot;
    options.maxOutputBytes = maxOutput;
    options.cancelled = [] { return gCancel.load(); };
    QElapsedTimer sinceLine;
    sinceLine.start();
    options.progress = [&](double fraction, int frames) {
        if (sinceLine.elapsed() < 250) // always throttled: a video that outruns its estimate would otherwise print a line a frame
            return;
        sinceLine.restart();
        line({{QStringLiteral("progress"), fraction}, {QStringLiteral("frames"), frames}});
    };
    const murasaki::import::ImportOutcome result = murasaki::import::runImport(*parsed.spec, options);
    if (!result.ok)
        return refuse(result.error);
    line({{QStringLiteral("done"), true},
          {QStringLiteral("id"), result.id},
          {QStringLiteral("dir"), result.directory},
          {QStringLiteral("file"), result.file},
          {QStringLiteral("frames"), result.frames},
          {QStringLiteral("variants"), result.variants},
          {QStringLiteral("width"), result.width},
          {QStringLiteral("height"), result.height},
          {QStringLiteral("totalDelayMs"), double(result.totalDelayMs)},
          {QStringLiteral("bytes"), double(result.bytes)},
          {QStringLiteral("reused"), result.reused},
          {QStringLiteral("degraded"), QJsonArray::fromStringList(result.degraded)}});
    return 0;
}
