#include "petaudiomanager.h"

#include "petaudiolimits.h"
#include "sandboxargs.h"
#include "sceneenginelookup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <ctime>

namespace murasaki {

using namespace Qt::StringLiterals;

namespace {

bool isId(const QString &id)
{
    if (id.size() != kPetAudioAssetIdLength)
        return false;
    for (const QChar c : id)
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    return true;
}

} // namespace

PetAudioManager::PetAudioManager(QString assetsDir, QString helper, QString sandbox, QObject *parent)
    : QObject(parent)
    , m_assetsDir(std::move(assetsDir))
    , m_helper(std::move(helper))
    , m_sandbox(std::move(sandbox))
{
}

PetAudioManager::~PetAudioManager()
{
    if (m_process) { // an import still running when the daemon ends: stopped, and it removes its own .part file when it gets the signal
        m_process->disconnect();
        m_process->terminate();
        if (!m_process->waitForFinished(2000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
}

PetAudioManager::Job *PetAudioManager::find(const QString &id)
{
    for (Job &job : m_jobs)
        if (job.id == id)
            return &job;
    return nullptr;
}
const PetAudioManager::Job *PetAudioManager::find(const QString &id) const
{
    for (const Job &job : m_jobs)
        if (job.id == id)
            return &job;
    return nullptr;
}

QString PetAudioManager::importFile(const QString &path, QString *error)
{
    const auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return QString();
    };
    if (!QDir::isAbsolutePath(path) || path.contains(QChar(0)) || path.size() > 4096)
        return refuse(u"spec refused: the sound must be an absolute path"_s);
    int waiting = 0;
    for (const Job &job : m_jobs)
        waiting += job.state == u"queued"_s || job.state == u"running"_s;
    if (waiting >= kMaxQueued)
        return refuse(u"too many sounds are being imported; wait for one to finish"_s);
    Job job;
    job.id = QString::number(++m_counter);
    job.path = QFileInfo(path).absoluteFilePath();
    job.name = QFileInfo(path).fileName().left(80);
    m_jobs.push_back(job);
    trim();
    emit changed(job.id);
    startNext();
    return job.id;
}

void PetAudioManager::trim()
{
    while (m_jobs.size() > size_t(kKeptJobs)) {
        bool dropped = false;
        for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
            if (it->state != u"queued"_s && it->state != u"running"_s) {
                m_jobs.erase(it);
                dropped = true;
                break;
            }
        }
        if (!dropped)
            break;
    }
}

void PetAudioManager::startNext()
{
    if (m_process)
        return;
    Job *next = nullptr;
    for (Job &job : m_jobs)
        if (job.state == u"queued"_s) {
            next = &job;
            break;
        }
    if (!next)
        return;
    const QString helper = !m_helper.isEmpty() ? m_helper : findPetAudioHelper();
    const Sandbox mode = m_sandboxMode ? m_sandboxMode() : Sandbox::Prefer;
    const QString sandbox = mode == Sandbox::Off ? QString() : (!m_sandbox.isEmpty() ? m_sandbox : findSandboxHelper());
    if (helper.isEmpty()) {
        next->state = u"failed"_s;
        next->error = u"helper failed: murasaki-petaudio is not installed"_s;
        emit changed(next->id);
        return startNext();
    }
    if (mode == Sandbox::Require && sandbox.isEmpty()) {
        next->state = u"failed"_s;
        next->error = u"sandbox unavailable"_s;
        emit changed(next->id);
        return startNext();
    }
    if (!QDir().mkpath(m_assetsDir)) {
        next->state = u"failed"_s;
        next->error = u"not enough disk space: cannot make the sounds folder"_s;
        emit changed(next->id);
        return startNext();
    }
    QStringList program;
    if (!sandbox.isEmpty()) {
        // The import profile of the pet imports: read the system, the helper and the one file, write the sounds folder, no TCP, and no runtime
        // directory in its view, so no session bus, Wayland or X11 socket can be reached.
        program << sandbox << murasaki::import::importSandboxArgs({next->path}, m_assetsDir, mode == Sandbox::Require) << u"--"_s;
    }
    program << helper << u"--import"_s << next->path << m_assetsDir << u"--name"_s << next->name << u"--deadline-ms"_s << QString::number(kDeadlineMs);
    m_process = std::make_unique<QProcess>();
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_output.clear();
    next->state = u"running"_s;
    m_running = next->id;
    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, [this] {
        if (m_output.size() < 65536)
            m_output += QString::fromUtf8(m_process->readAllStandardOutput());
    });
    connect(m_process.get(), &QProcess::finished, this, &PetAudioManager::finished);
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finished(127, QProcess::CrashExit);
    });
    const QString exe = program.takeFirst();
    m_process->start(exe, program);
    emit changed(next->id);
}

void PetAudioManager::finished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    const QString id = m_running;
    m_process.release()->deleteLater();
    m_running.clear();
    if (Job *job = find(id)) {
        const QJsonObject result = QJsonDocument::fromJson(m_output.section(u'\n', 0, 0).toUtf8()).object();
        job->result = result;
        if (job->state == u"cancelled"_s) {
            // stays cancelled
        } else if (status == QProcess::NormalExit && exitCode == 0 && result.value(u"ok"_s).toBool()) {
            job->state = u"done"_s;
        } else if (result.value(u"cancelled"_s).toBool()) {
            job->state = u"cancelled"_s;
        } else {
            job->state = u"failed"_s;
            const QString reported = result.value(u"error"_s).toString();
            job->error = !reported.isEmpty() ? reported : u"helper failed"_s;
        }
        emit changed(id);
        emit changed(QString());
    }
    startNext();
}

QJsonObject PetAudioManager::job(const QString &id) const
{
    const Job *j = find(id);
    if (!j)
        return {};
    QJsonObject o{{u"id"_s, j->id}, {u"state"_s, j->state}, {u"name"_s, j->name}};
    if (!j->error.isEmpty())
        o.insert(u"error"_s, j->error);
    if (!j->result.isEmpty())
        o.insert(u"result"_s, j->result);
    return o;
}

bool PetAudioManager::cancel(const QString &id)
{
    Job *j = find(id);
    if (!j)
        return false;
    if (j->state == u"queued"_s) {
        j->state = u"cancelled"_s;
        emit changed(id);
        return true;
    }
    if (j->state == u"running"_s && m_process) {
        j->state = u"cancelled"_s;
        m_process->terminate(); // the helper stops at its next poll and removes its .part file
        emit changed(id);
        return true;
    }
    return false;
}

QJsonArray PetAudioManager::assets(const QStringList &referenced) const
{
    QJsonArray out;
    const QDir dir(m_assetsDir);
    for (const QString &name : dir.entryList({u"*.wav"_s}, QDir::Files, QDir::Name)) {
        const QString id = name.chopped(4);
        if (!isId(id))
            continue;
        QJsonObject o{{u"asset"_s, id}, {u"bytes"_s, QFileInfo(dir.filePath(name)).size()}, {u"refs"_s, int(referenced.count(id))}};
        QFile meta(dir.filePath(id + u".json"_s));
        if (meta.open(QIODevice::ReadOnly) && meta.size() < 65536) {
            const QJsonObject m = QJsonDocument::fromJson(meta.readAll()).object();
            for (const QString &key : {u"name"_s, u"durationMs"_s, u"sourceFormat"_s})
                if (m.contains(key))
                    o.insert(key, m.value(key));
        }
        out.append(o);
    }
    return out;
}

bool PetAudioManager::removeAsset(const QString &asset, const QStringList &referenced, QString *error)
{
    const auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!isId(asset))
        return refuse(u"audio asset missing: not an asset id"_s);
    if (referenced.contains(asset))
        return refuse(u"the sound is used by a pet"_s);
    const QDir dir(m_assetsDir);
    if (!QFileInfo::exists(dir.filePath(asset + u".wav"_s)))
        return refuse(u"audio asset missing"_s);
    QFile::remove(dir.filePath(asset + u".json"_s));
    if (!QFile::remove(dir.filePath(asset + u".wav"_s)))
        return refuse(u"cannot remove the sound"_s);
    emit changed(QString());
    return true;
}

int PetAudioManager::collect(const QStringList &referenced, long long minAgeSeconds)
{
    int removed = 0;
    const QDir dir(m_assetsDir);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QStringList names = dir.entryList({u"*.wav"_s, u"*.json"_s, u"*.part"_s}, QDir::Files);
    for (const QString &name : names) {
        QString id = name;
        for (const QString &suffix : {u".wav.part"_s, u".json.part"_s, u".wav"_s, u".json"_s})
            if (name.endsWith(suffix)) {
                id = name.chopped(suffix.size());
                break;
            }
        if (!isId(id) || referenced.contains(id) || (m_process && name.endsWith(u".part"_s)))
            continue; // (a partial file belongs to the import that is running)
        const QFileInfo info(dir.filePath(name));
        if (now - info.lastModified().toSecsSinceEpoch() < minAgeSeconds)
            continue;
        if (QFile::remove(dir.filePath(name)) && name.endsWith(u".wav"_s))
            ++removed;
    }
    if (removed > 0)
        emit changed(QString());
    return removed;
}

} // namespace murasaki
