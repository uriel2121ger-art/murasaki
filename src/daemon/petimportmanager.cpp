#include "petimportmanager.h"

#include "importfolders.h"
#include "importspec.h"
#include "petimport.h"
#include "sandboxargs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

using namespace murasaki::import;

namespace murasaki {

namespace {

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

const char *stateName(PetImportManager::State s)
{
    switch (s) {
    case PetImportManager::State::Queued: return "queued";
    case PetImportManager::State::Running: return "running";
    case PetImportManager::State::Done: return "done";
    case PetImportManager::State::Failed: return "failed";
    case PetImportManager::State::Cancelled: return "cancelled";
    }
    return "failed";
}

const char *kindName(ImportKind k)
{
    switch (k) {
    case ImportKind::Sheet: return "sheet";
    case ImportKind::Images: return "images";
    case ImportKind::Zip: return "zip";
    case ImportKind::Gif: return "gif";
    case ImportKind::Mdaa: return "mdaa";
    case ImportKind::Video: return "video";
    }
    return "sheet";
}

constexpr int kSandboxFailed = 125; // murasaki-sandbox: usage error or no confinement while --require

} // namespace

PetImportManager::PetImportManager(QString outRoot, QObject *parent) : QObject(parent), m_outRoot(std::move(outRoot))
{
    m_wallClock.setSingleShot(true);
    m_killTimer.setSingleShot(true);
    m_previewTimer.setSingleShot(true);
    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        if (m_previewProcess)
            m_previewProcess->kill();
    });
    connect(&m_wallClock, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        if (m_process)
            m_process->terminate();
        m_killTimer.start(5000);
    });
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_process)
            m_process->kill();
    });
}

PetImportManager::~PetImportManager()
{
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

void PetImportManager::setHelper(QString helper, QString sandbox, bool requireSandbox)
{
    m_helper = std::move(helper);
    m_sandbox = std::move(sandbox);
    m_requireSandbox = requireSandbox;
}

PetImportManager::Job *PetImportManager::find(const QString &jobId)
{
    for (Job &j : m_jobs)
        if (j.id == jobId)
            return &j;
    return nullptr;
}

const PetImportManager::Job *PetImportManager::find(const QString &jobId) const
{
    return const_cast<PetImportManager *>(this)->find(jobId);
}

QString PetImportManager::submit(const QByteArray &specJson, QString *error)
{
    const SpecResult parsed = parseImportSpec(specJson);
    if (!parsed.ok()) {
        *error = parsed.error;
        return {};
    }
    int unfinished = 0;
    for (const Job &j : std::as_const(m_jobs))
        unfinished += j.state == State::Queued || j.state == State::Running;
    if (unfinished >= kMaxQueued) {
        *error = named(kSpecRefused, QStringLiteral("too many imports waiting"));
        return {};
    }
    Job job;
    job.id = QStringLiteral("imp-%1-%2").arg(++m_counter).arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    job.kind = QLatin1String(kindName(parsed.spec->kind));
    job.name = parsed.spec->name.left(200);
    job.sources = parsed.spec->sources;
    job.specJson = specJson;
    m_jobs.append(job);
    emit changed(job.id);
    startNext();
    return job.id;
}

QStringList PetImportManager::commandFor(const Job &job, const QString &specPath, QString *program) const
{
    QStringList args;
    if (m_sandbox.isEmpty()) {
        *program = m_helper;
    } else {
        *program = m_sandbox;
        QStringList reads = job.sources;
        reads.append(specPath);
        args = importSandboxArgs(reads, m_outRoot, m_requireSandbox);
        args << QStringLiteral("--") << m_helper;
    }
    args << QStringLiteral("--spec") << specPath << QStringLiteral("--out-root") << m_outRoot << QStringLiteral("--max-output") << QString::number(m_maxOutput);
    return args;
}

void PetImportManager::startNext()
{
    if (m_process)
        return;
    Job *next = nullptr;
    for (Job &j : m_jobs) {
        if (j.state == State::Queued) {
            next = &j;
            break;
        }
    }
    if (!next)
        return;
    if (m_helper.isEmpty() || !QFileInfo(m_helper).isExecutable() || (m_sandbox.isEmpty() && m_requireSandbox)) {
        finish(*next, State::Failed, named(m_sandbox.isEmpty() && m_requireSandbox ? kSandboxUnavailable : kHelperFailed, QStringLiteral("no helper to run")));
        QTimer::singleShot(0, this, &PetImportManager::startNext);
        return;
    }
    const QString specDir = m_outRoot + QStringLiteral("/.specs");
    const QString specPath = specDir + QLatin1Char('/') + next->id + QStringLiteral(".json");
    QFile spec(specPath);
    if (!QDir().mkpath(specDir) || !spec.open(QIODevice::WriteOnly) || spec.write(next->specJson) != next->specJson.size()) {
        finish(*next, State::Failed, named(kHelperFailed, QStringLiteral("the spec cannot be written")));
        QTimer::singleShot(0, this, &PetImportManager::startNext);
        return;
    }
    spec.close();
    QString program;
    const QStringList args = commandFor(*next, specPath, &program);
    m_process = std::make_unique<QProcess>();
    m_runningId = next->id;
    m_buffer.clear();
    m_stderr.clear();
    m_sawDone = m_sawError = false;
    m_timedOut = false;
    next->unconfined = m_sandbox.isEmpty();
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, &PetImportManager::onOutput);
    connect(m_process.get(), &QProcess::readyReadStandardError, this, [this] { m_stderr = (m_stderr + m_process->readAllStandardError()).right(2000); });
    connect(m_process.get(), &QProcess::finished, this, &PetImportManager::onFinished);
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            onFinished(-1, QProcess::CrashExit);
    });
    next->state = State::Running;
    m_lastEmit.restart();
    m_process->start(program, args);
    m_wallClock.start(m_wallMs);
    emit changed(next->id);
}

void PetImportManager::onOutput()
{
    m_buffer += m_process->readAllStandardOutput();
    Job *job = find(m_runningId);
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (!job || line.size() > 64 * 1024)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.contains(QLatin1String("progress"))) {
            const double fraction = o.value(QLatin1String("progress")).toDouble();
            job->progress = fraction < 0 ? -1.0 : qBound(0.0, fraction, 1.0); // -1: not known
            job->frames = o.value(QLatin1String("frames")).toInt();
            if (m_lastEmit.elapsed() >= 250) {
                m_lastEmit.restart();
                emit changed(job->id);
            }
        } else if (o.value(QLatin1String("done")).toBool()) {
            m_sawDone = true;
            job->file = o.value(QLatin1String("file")).toString();
            job->frames = o.value(QLatin1String("frames")).toInt();
            job->variants = o.value(QLatin1String("variants")).toInt();
            for (const QJsonValue &d : o.value(QLatin1String("degraded")).toArray())
                job->degraded.append(d.toString().left(200));
        } else if (o.contains(QLatin1String("error"))) {
            m_sawError = true;
            job->error = o.value(QLatin1String("error")).toString().left(400);
        }
    }
    if (m_buffer.size() > 64 * 1024)
        m_buffer.clear();
}

void PetImportManager::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    onOutput();
    m_wallClock.stop();
    m_killTimer.stop();
    const QString id = m_runningId;
    m_runningId.clear();
    std::unique_ptr<QProcess> process = std::move(m_process);
    process->deleteLater();
    process.release();
    QFile::remove(m_outRoot + QStringLiteral("/.specs/") + id + QStringLiteral(".json"));
    if (Job *job = find(id)) {
        const QString stderrTail = QString::fromUtf8(m_stderr).trimmed().right(200);
        // The sandbox says once, on stderr, when it ran the program without confinement (importRequireSandbox off and no Landlock).
        if (m_stderr.contains("unconfined"))
            job->unconfined = true;
        if (job->unconfined && !job->degraded.contains(named(kNotSandboxed)))
            job->degraded.append(named(kNotSandboxed));
        if (job->cancelRequested)
            finish(*job, State::Cancelled, named(kCancelled));
        else if (m_timedOut)
            finish(*job, State::Failed, named(kTimeBudget, QStringLiteral("stopped after the wall-clock limit")));
        else if (m_sawDone && exitCode == 0 && status == QProcess::NormalExit)
            finish(*job, State::Done, {});
        else if (m_sawError)
            finish(*job, State::Failed, job->error);
        else if (exitCode == kSandboxFailed && m_requireSandbox)
            finish(*job, State::Failed, named(kSandboxUnavailable));
        else
            finish(*job, State::Failed, named(kHelperFailed, stderrTail));
    }
    QTimer::singleShot(0, this, &PetImportManager::startNext);
}

void PetImportManager::finish(Job &job, State state, const QString &error)
{
    job.state = state;
    job.error = error;
    job.specJson.clear(); // (up to 2 MiB of paths a job no longer needs; 50 finished jobs are kept)
    job.specJson.squeeze();
    if (state == State::Done)
        job.progress = 1;
    const QString id = job.id;
    const QString file = job.file;
    emit changed(id);
    if (state == State::Done)
        emit done(id, file);
    trim();
}

void PetImportManager::trim()
{
    int finished = 0;
    for (const Job &j : std::as_const(m_jobs))
        finished += j.state != State::Queued && j.state != State::Running;
    for (int i = 0; i < m_jobs.size() && finished > kKeepFinished; ++i) {
        if (m_jobs.at(i).state != State::Queued && m_jobs.at(i).state != State::Running) {
            m_jobs.removeAt(i--);
            --finished;
        }
    }
}

bool PetImportManager::cancel(const QString &jobId)
{
    Job *job = find(jobId);
    if (!job)
        return false;
    if (job->state == State::Queued) {
        finish(*job, State::Cancelled, named(kCancelled));
        return true;
    }
    if (job->state == State::Running && m_process && m_runningId == jobId) {
        job->cancelRequested = true;
        m_process->terminate();
        m_killTimer.start(5000);
        return true;
    }
    return true; // already finished: nothing to stop (a window that closes just after the job ended is not an error)
}

QJsonObject PetImportManager::job(const QString &jobId) const
{
    const Job *j = find(jobId);
    if (!j)
        return {};
    QJsonObject o{{QStringLiteral("id"), j->id},
                  {QStringLiteral("kind"), j->kind},
                  {QStringLiteral("name"), j->name},
                  {QStringLiteral("state"), QLatin1String(stateName(j->state))},
                  {QStringLiteral("progress"), j->progress},
                  {QStringLiteral("frames"), j->frames},
                  {QStringLiteral("degraded"), QJsonArray::fromStringList(j->degraded)}};
    if (!j->error.isEmpty())
        o.insert(QStringLiteral("error"), j->error);
    if (!j->itemId.isEmpty())
        o.insert(QStringLiteral("itemId"), j->itemId);
    if (j->variants > 0)
        o.insert(QStringLiteral("variants"), j->variants);
    return o;
}

QJsonArray PetImportManager::jobs() const
{
    QJsonArray all;
    for (const Job &j : m_jobs)
        all.append(job(j.id));
    return all;
}

void PetImportManager::setItemId(const QString &jobId, const QString &itemId)
{
    if (Job *j = find(jobId)) {
        j->itemId = itemId;
        emit changed(jobId);
    }
}

void PetImportManager::markFailed(const QString &jobId, const QString &error)
{
    if (Job *j = find(jobId)) {
        j->state = State::Failed;
        j->error = error;
        emit changed(jobId);
    }
}

void PetImportManager::cleanStale()
{
    cleanStaleImportParts(m_outRoot);
    const QDir specs(m_outRoot + QStringLiteral("/.specs"));
    for (const QString &name : specs.entryList(QDir::Files))
        QFile::remove(specs.filePath(name));
    // The locks of imports that died with their process (.<id>.lock; a live lock is rebuilt by its owner).
    const QDir root(m_outRoot);
    for (const QString &name : root.entryList({QStringLiteral(".*.lock")}, QDir::Files | QDir::Hidden))
        QFile::remove(root.filePath(name));
}

namespace {

QString previewFailure(const QString &message)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}}).toJson(QJsonDocument::Compact));
}

// The first line of the helper's output as the answer of a preview.
QString previewAnswer(const QByteArray &output, int exitCode, bool requireSandbox)
{
    const QJsonObject o = QJsonDocument::fromJson(output.split('\n').value(0)).object();
    if (o.contains(QLatin1String("preview")))
        return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("preview"), o.value(QLatin1String("preview"))}}).toJson(QJsonDocument::Compact));
    if (o.contains(QLatin1String("error")))
        return previewFailure(o.value(QLatin1String("error")).toString().left(400));
    return previewFailure(named(exitCode == kSandboxFailed && requireSandbox ? kSandboxUnavailable : kHelperFailed));
}

} // namespace

bool PetImportManager::previewAsync(const QByteArray &specJson, std::function<void(const QString &)> done, QString *error)
{
    const SpecResult parsed = parseImportSpec(specJson);
    if (!parsed.ok()) {
        *error = parsed.error;
        return false;
    }
    for (PendingPreview &p : m_previews) {
        if (p.spec == specJson) { // the same question is already waiting or running: share its answer
            p.waiting.append(std::move(done));
            return true;
        }
    }
    if (m_previews.size() >= kMaxPendingPreviews) {
        done(previewFailure(QStringLiteral("busy: too many previews are waiting")));
        return true;
    }
    m_previews.append({specJson, {std::move(done)}});
    startNextPreview();
    return true;
}

void PetImportManager::startNextPreview()
{
    if (m_previews.isEmpty() || m_previewProcess)
        return;
    const QByteArray spec = m_previews.first().spec;
    const SpecResult parsed = parseImportSpec(spec);
    if (m_helper.isEmpty() || !QFileInfo(m_helper).isExecutable() || (m_sandbox.isEmpty() && m_requireSandbox)) {
        finishPreview(previewFailure(named(m_sandbox.isEmpty() && m_requireSandbox ? kSandboxUnavailable : kHelperFailed, QStringLiteral("no helper to run"))));
        return;
    }
    const QString specDir = m_outRoot + QStringLiteral("/.specs");
    m_previewSpecFile = specDir + QLatin1Char('/') + QStringLiteral("preview-%1.json").arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    QFile file(m_previewSpecFile);
    if (!QDir().mkpath(specDir) || !file.open(QIODevice::WriteOnly) || file.write(spec) != spec.size()) {
        finishPreview(previewFailure(named(kHelperFailed, QStringLiteral("the spec cannot be written"))));
        return;
    }
    file.close();
    Job probe;
    probe.sources = parsed.spec->sources;
    probe.sources.append(m_previewSpecFile);
    QString program;
    QStringList args = commandFor(probe, m_previewSpecFile, &program);
    args << QStringLiteral("--preview");
    m_previewProcess = std::make_unique<QProcess>();
    connect(m_previewProcess.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QByteArray output = m_previewProcess->readAllStandardOutput();
        finishPreview(status == QProcess::NormalExit ? previewAnswer(output, code, m_requireSandbox) : previewFailure(named(kHelperFailed, QStringLiteral("the preview took too long"))));
    });
    connect(m_previewProcess.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finishPreview(previewFailure(named(kHelperFailed)));
    });
    m_previewProcess->start(program, args);
    m_previewTimer.start(8000);
}

void PetImportManager::finishPreview(const QString &answer)
{
    m_previewTimer.stop();
    if (m_previewProcess) {
        m_previewProcess.release()->deleteLater();
    }
    if (!m_previewSpecFile.isEmpty())
        QFile::remove(m_previewSpecFile);
    m_previewSpecFile.clear();
    if (m_previews.isEmpty())
        return;
    const PendingPreview done = m_previews.takeFirst();
    for (const auto &callback : done.waiting)
        callback(answer);
    QTimer::singleShot(0, this, &PetImportManager::startNextPreview);
}

QString PetImportManager::preview(const QByteArray &specJson, QString *error)
{
    const SpecResult parsed = parseImportSpec(specJson);
    if (!parsed.ok()) {
        *error = parsed.error;
        return {};
    }
    const auto fail = [](const QString &message) {
        return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}}).toJson(QJsonDocument::Compact));
    };
    if (m_helper.isEmpty() || !QFileInfo(m_helper).isExecutable() || (m_sandbox.isEmpty() && m_requireSandbox))
        return fail(named(m_sandbox.isEmpty() && m_requireSandbox ? kSandboxUnavailable : kHelperFailed, QStringLiteral("no helper to run")));
    const QString specDir = m_outRoot + QStringLiteral("/.specs");
    const QString specPath = specDir + QLatin1Char('/') + QStringLiteral("preview-%1.json").arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    QFile spec(specPath);
    if (!QDir().mkpath(specDir) || !spec.open(QIODevice::WriteOnly) || spec.write(specJson) != specJson.size())
        return fail(named(kHelperFailed, QStringLiteral("the spec cannot be written")));
    spec.close();
    Job probe;
    probe.sources = parsed.spec->sources;
    QString program;
    QStringList args = commandFor(probe, specPath, &program);
    args << QStringLiteral("--preview");
    QProcess process;
    process.start(program, args);
    QString result;
    if (!process.waitForFinished(8000)) {
        process.kill();
        process.waitForFinished(2000);
        result = fail(named(kHelperFailed, QStringLiteral("the preview took too long")));
    } else {
        const QJsonObject o = QJsonDocument::fromJson(process.readAllStandardOutput().split('\n').value(0)).object();
        if (o.contains(QLatin1String("preview")))
            result = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("preview"), o.value(QLatin1String("preview"))}}).toJson(QJsonDocument::Compact));
        else if (o.contains(QLatin1String("error")))
            result = fail(o.value(QLatin1String("error")).toString().left(400));
        else
            result = fail(named(process.exitCode() == kSandboxFailed && m_requireSandbox ? kSandboxUnavailable : kHelperFailed));
    }
    QFile::remove(specPath);
    return result;
}

} // namespace murasaki
