#include "sceneengine.h"

#include "constants.h"
#include "jsonfile.h"
#include "sandboxreport.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace murasaki {

namespace {

// "0.5", "12", "1.25": never an exponent, which the engine's number parser does not accept.
QString formatNumber(double value, bool keepDot)
{
    if (!std::isfinite(value))
        return QStringLiteral("0");
    if (!keepDot && value == std::trunc(value) && std::fabs(value) < 1e15)
        return QString::number(static_cast<qlonglong>(value));
    return QString::number(value, 'f', 6);
}

QString safeToken(const QString &value, const QString &fallback)
{
    if (value.isEmpty())
        return fallback;
    // A token starting with '-' would be taken for an option.
    return value.startsWith(QLatin1Char('-')) ? QStringLiteral("./") + value : value;
}

} // namespace


QSize scaledBufferSize(const QSize &pixels, double scale)
{
    if (pixels.isEmpty()) {
        return pixels;
    }
    const double s = std::clamp(std::isfinite(scale) ? scale : 1.0, kMinSceneRenderScale, kMaxSceneRenderScale);
    const auto side = [s](int pixelsOnSide) {
        const int floor = std::min(pixelsOnSide, 64);
        int scaled = static_cast<int>(std::lround(pixelsOnSide * s));
        scaled -= scaled % 2; // video-friendly, and buffers like even sizes
        return std::min(pixelsOnSide, std::max(scaled, std::max(floor, 2))); // not std::clamp: a one pixel side has lo > hi
    };
    return {side(pixels.width()), side(pixels.height())};
}

SandboxedCommand sandboxSceneEngine(const QString &helper, const QString &itemDir, const QString &assetsDir,
                                    const QString &program, const QStringList &arguments, bool require)
{
    SandboxedCommand command;
    command.program = helper;
    command.arguments << QStringLiteral("--profile") << QStringLiteral("scene") << QStringLiteral("--item")
                      << safeToken(itemDir, QStringLiteral("."));
    if (!assetsDir.isEmpty())
        command.arguments << QStringLiteral("--assets") << safeToken(assetsDir, QString());
    if (require)
        command.arguments << QStringLiteral("--require");
    command.arguments << QStringLiteral("--") << program << arguments;
    return command;
}

bool sceneOutputNameValid(const QString &name)
{
    if (name.startsWith(QLatin1Char('-')))
        return false;
    return std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
}

std::optional<QString> scenePropertyValue(const QVariant &value)
{
    const auto type = static_cast<QMetaType::Type>(value.typeId());
    if (type == QMetaType::Bool)
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (type == QMetaType::QString)
        return value.toString();
    if (bridgeIsNumeric(type))
        return formatNumber(value.toDouble(), false);
    if (type == QMetaType::QVariantList) {
        const QVariantList list = value.toList();
        if (list.isEmpty())
            return std::nullopt;
        QStringList parts;
        for (const QVariant &component : list) {
            const auto ct = static_cast<QMetaType::Type>(component.typeId());
            if (!bridgeIsNumeric(ct))
                return std::nullopt;
            parts << formatNumber(component.toDouble(), bridgeIsFloating(ct));
        }
        return parts.join(QLatin1Char(' '));
    }
    return std::nullopt;
}

QStringList sceneEngineArguments(const SceneLaunch &launch)
{
    QStringList args;
    args << QStringLiteral("--murasaki-bridge-fd") << QString::number(launch.bridgeFd)
         << QStringLiteral("--murasaki-size")
         << QStringLiteral("%1x%2").arg(std::max(1, launch.size.width())).arg(std::max(1, launch.size.height()));
    if (!launch.assetsDir.isEmpty())
        args << QStringLiteral("--assets-dir") << safeToken(launch.assetsDir, QString());
    // Never --silent: it makes the engine skip loading the scene's sounds, so unmuting would need a restart. A
    // volume of 0 keeps everything loaded and the Volume message changes it live.
    args << QStringLiteral("--volume") << QString::number(std::clamp(launch.volume, 0, 128));
    args << QStringLiteral("--fps") << QString::number(std::max(1, launch.fps));
    const QString mode = launch.fill == QLatin1String("fit") || launch.fill == QLatin1String("stretch")
        ? launch.fill
        : QStringLiteral("fill");
    args << QStringLiteral("--screen-root") << (sceneOutputNameValid(launch.outputName) && !launch.outputName.isEmpty() ? launch.outputName
                                                                                      : QStringLiteral("murasaki"))
         << QStringLiteral("--bg") << safeToken(launch.itemDir, QStringLiteral("."))
         << QStringLiteral("--scaling") << mode;
    if (launch.renderScale > 0.0)
        args << QStringLiteral("--render-scale") << QString::number(launch.renderScale, 'g', 4);
    for (auto it = launch.properties.cbegin(); it != launch.properties.cend(); ++it) {
        if (!bridgePropertyKeyUsable(it.key()))
            continue;
        if (const auto text = scenePropertyValue(it.value()))
            args << QStringLiteral("--set-property") << it.key() + QLatin1Char('=') + *text;
    }
    return args;
}

// ---- choosing the engine -------------------------------------------------------------------------------------

namespace {

constexpr int kCacheVersion = 1;

QString firstReason(const SceneCheckVerdict &verdict)
{
    return verdict.reasons.isEmpty() ? QStringLiteral("not supported yet") : verdict.reasons.first();
}

} // namespace

SceneEngineMode sceneEngineModeFromString(const QString &text)
{
    if (text == kSceneEngineAuto)
        return SceneEngineMode::Auto;
    if (text == kSceneEngineVulkan)
        return SceneEngineMode::Vulkan;
    return SceneEngineMode::Auto;
}

std::optional<SceneCheckVerdict> parseSceneCheckOutput(const QByteArray &output, QString *error)
{
    const auto fail = [error](const QString &why) -> std::optional<SceneCheckVerdict> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    // The verdict is the last non-empty line.
    const QList<QByteArray> lines = output.split('\n');
    QByteArray line;
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        if (!it->trimmed().isEmpty()) {
            line = it->trimmed();
            break;
        }
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() || !doc.object().value(QStringLiteral("ok")).isBool())
        return fail(QStringLiteral("--check printed no verdict"));
    SceneCheckVerdict verdict;
    verdict.ok = doc.object().value(QStringLiteral("ok")).toBool();
    for (const QJsonValue &reason : doc.object().value(QStringLiteral("reasons")).toArray())
        verdict.reasons << reason.toString();
    if (!verdict.ok && verdict.reasons.isEmpty())
        verdict.reasons << QStringLiteral("not supported");
    return verdict;
}

QString sceneCheckKey(const QString &itemDir, const QString &enginePath, const QString &assetsDir)
{
    // The newest modification time anywhere in the item's tree: the folder's own mtime catches added and removed
    // files, the files' and subfolders' catch edits in place (a package replaced by an update). Bounded, so a
    // pathological tree cannot make a launch slow.
    const QFileInfo dir(itemDir);
    qint64 newest = dir.lastModified().toMSecsSinceEpoch();
    int seen = 0;
    QDirIterator it(itemDir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && seen++ < 2000) {
        it.next();
        newest = std::max(newest, it.fileInfo().lastModified().toMSecsSinceEpoch());
    }
    const QFileInfo engine(enginePath);
    const QFileInfo assets(assetsDir);
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
        .arg(dir.absoluteFilePath())
        .arg(newest)
        .arg(engine.absoluteFilePath())
        .arg(engine.size())
        .arg(engine.lastModified().toMSecsSinceEpoch())
        .arg(assetsDir.isEmpty() ? QString() : assets.absoluteFilePath())
        .arg(assetsDir.isEmpty() ? 0 : assets.lastModified().toMSecsSinceEpoch());
}

SceneCheckCache::SceneCheckCache(QString path, int maxEntries)
    : m_path(std::move(path))
    , m_maxEntries(maxEntries)
{
}

QString SceneCheckCache::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/murasaki/scene-check.json");
}

SceneCheckCache &SceneCheckCache::shared()
{
    static SceneCheckCache cache;
    return cache;
}

void SceneCheckCache::load() const
{
    if (m_loaded)
        return;
    m_loaded = true;
    // The cache is disposable: a missing, unreadable or foreign file is simply an empty cache.
    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists || !read.problem.isEmpty() || read.root.value(QStringLiteral("version")).toInt() != kCacheVersion)
        return;
    const QJsonObject entries = read.root.value(QStringLiteral("entries")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Entry entry;
        entry.dir = o.value(QStringLiteral("dir")).toString();
        entry.crashes = std::max(0, o.value(QStringLiteral("crashes")).toInt());
        entry.failure = o.value(QStringLiteral("failure")).toString();
        entry.failedAtMs = static_cast<qint64>(o.value(QStringLiteral("failedAt")).toDouble());
        if (o.value(QStringLiteral("ok")).isBool()) {
            entry.hasVerdict = true;
            entry.verdict.ok = o.value(QStringLiteral("ok")).toBool();
            for (const QJsonValue &r : o.value(QStringLiteral("reasons")).toArray())
                entry.verdict.reasons << r.toString();
        }
        m_entries.insert(it.key(), entry);
    }
}

void SceneCheckCache::save() const
{
    // Bounded: drop arbitrary entries beyond the limit rather than grow without end.
    QJsonObject entries;
    int count = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend() && count < m_maxEntries; ++it, ++count) {
        QJsonObject o{{QStringLiteral("dir"), it->dir}, {QStringLiteral("crashes"), it->crashes}};
        if (!it->failure.isEmpty()) {
            o.insert(QStringLiteral("failure"), it->failure);
            o.insert(QStringLiteral("failedAt"), static_cast<double>(it->failedAtMs));
        }
        if (it->hasVerdict) {
            o.insert(QStringLiteral("ok"), it->verdict.ok);
            o.insert(QStringLiteral("reasons"), QJsonArray::fromStringList(it->verdict.reasons));
        }
        entries.insert(it.key(), o);
    }
    writeJsonObjectFile(m_path, QJsonObject{{QStringLiteral("version"), kCacheVersion}, {QStringLiteral("entries"), entries}});
}

void SceneCheckCache::forgetOtherKeysOf(const QString &dir, const QString &key) const
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it.key() != key && it->dir == dir)
            it = m_entries.erase(it);
        else
            ++it;
    }
}

std::optional<SceneCheckVerdict> SceneCheckCache::verdict(const QString &key) const
{
    load();
    const auto it = m_entries.constFind(key);
    if (it == m_entries.cend() || !it->hasVerdict)
        return std::nullopt;
    return it->verdict;
}

void SceneCheckCache::storeVerdict(const QString &itemDir, const QString &key, const SceneCheckVerdict &verdict)
{
    load();
    forgetOtherKeysOf(itemDir, key);
    Entry &entry = m_entries[key];
    entry.dir = itemDir;
    entry.hasVerdict = true;
    entry.verdict = verdict;
    entry.failure.clear();
    save();
}

void SceneCheckCache::storeFailure(const QString &itemDir, const QString &key, const QString &reason, qint64 atMs)
{
    load();
    forgetOtherKeysOf(itemDir, key);
    Entry &entry = m_entries[key];
    entry.dir = itemDir;
    entry.failure = reason.isEmpty() ? QStringLiteral("check failed") : reason;
    entry.failedAtMs = atMs >= 0 ? atMs : QDateTime::currentMSecsSinceEpoch();
    save();
}

std::optional<QString> SceneCheckCache::failure(const QString &key, qint64 nowMs) const
{
    load();
    const auto it = m_entries.constFind(key);
    if (it == m_entries.cend() || it->failure.isEmpty())
        return std::nullopt;
    const qint64 now = nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
    if (now - it->failedAtMs >= kCheckFailureTtlMs || now < it->failedAtMs - kCheckFailureTtlMs)
        return std::nullopt; // stale (or from a clock far in the future): ask again
    return it->failure;
}

int SceneCheckCache::crashes(const QString &key) const
{
    load();
    const auto it = m_entries.constFind(key);
    return it == m_entries.cend() ? 0 : it->crashes;
}

int SceneCheckCache::recordCrash(const QString &itemDir, const QString &key)
{
    load();
    forgetOtherKeysOf(itemDir, key);
    Entry &entry = m_entries[key];
    entry.dir = itemDir;
    ++entry.crashes;
    const int count = entry.crashes;
    save();
    return count;
}

// The process and the timer are children of the runner and are released with deleteLater(): a job ends from inside
// one of their own signals.
struct SceneCheckRunner::Job {
    QString itemDir;
    QProcess *process = nullptr;
    QTimer *timeout = nullptr;
};

SceneCheckRunner::SceneCheckRunner(SceneCheckCache *cache, int timeoutMs, QObject *parent)
    : QObject(parent)
    , m_cache(cache ? cache : &SceneCheckCache::shared())
    , m_timeoutMs(timeoutMs)
{
}

SceneCheckRunner::~SceneCheckRunner()
{
    for (Job *job : std::as_const(m_jobs)) {
        job->process->disconnect();
        job->process->kill();
        job->process->waitForFinished(1000);
        delete job; // the process and timer are children of this object and go with it
    }
}

SceneCheckRunner &SceneCheckRunner::shared()
{
    static SceneCheckRunner runner(&SceneCheckCache::shared());
    return runner;
}

bool SceneCheckRunner::running(const QString &key) const
{
    return m_jobs.contains(key) || std::any_of(m_queue.cbegin(), m_queue.cend(), [&key](const Queued &q) { return q.key == key; });
}

void SceneCheckRunner::request(const QString &itemDir, const QString &assetsDir, const QString &engine, const QString &key)
{
    if (m_jobs.contains(key))
        return; // single flight
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        if (m_queue[i].key == key) {
            m_queue.move(i, 0); // asked again: it is the newest
            return;
        }
    }
    if (m_jobs.size() >= kMaxParallelChecks) {
        m_queue.prepend({itemDir, assetsDir, engine, key});
        while (m_queue.size() > kMaxQueuedChecks)
            m_queue.removeLast();
        return;
    }
    start({itemDir, assetsDir, engine, key});
}

void SceneCheckRunner::startNext()
{
    while (m_jobs.size() < kMaxParallelChecks && !m_queue.isEmpty())
        start(m_queue.takeFirst());
}

void SceneCheckRunner::start(const Queued &what)
{
    const QString &itemDir = what.itemDir;
    const QString &assetsDir = what.assetsDir;
    const QString &engine = what.engine;
    const QString &key = what.key;
    auto *job = new Job;
    job->itemDir = itemDir;
    job->process = new QProcess(this);
    job->timeout = new QTimer(this);
    m_jobs.insert(key, job);
    job->process->setProcessChannelMode(QProcess::SeparateChannels);
    job->timeout->setSingleShot(true);
    connect(job->timeout, &QTimer::timeout, this, [this, key, job, engine] {
        job->process->disconnect(this);
        job->process->kill();
        finish(key, std::nullopt, QStringLiteral("%1 --check timed out").arg(engine));
    });
    connect(job->process, &QProcess::errorOccurred, this, [this, key, engine](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(key, std::nullopt, QStringLiteral("cannot run %1").arg(engine));
    });
    connect(job->process, &QProcess::finished, this, [this, key, job, engine](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0) {
            finish(key, std::nullopt, QStringLiteral("%1 --check failed (exit code %2)").arg(engine).arg(code));
            return;
        }
        QString error;
        const auto verdict = parseSceneCheckOutput(job->process->readAllStandardOutput(), &error);
        finish(key, verdict, verdict ? QString() : QStringLiteral("%1 %2").arg(engine, error));
    });
    QStringList args{QStringLiteral("--check"), itemDir.startsWith(QLatin1Char('-')) ? QStringLiteral("./") + itemDir : itemDir};
    if (!assetsDir.isEmpty()) {
        args << QStringLiteral("--assets-dir")
             << (assetsDir.startsWith(QLatin1Char('-')) ? QStringLiteral("./") + assetsDir : assetsDir);
    }
    QString program = engine;
    if (m_sandbox || m_require) {
        const QString helper = findSandboxHelper();
        if (!helper.isEmpty() && (!m_require || querySandboxReport(helper).confines())) {
            const SandboxedCommand command = sandboxSceneEngine(helper, itemDir, assetsDir, engine, args, m_require);
            program = command.program;
            args = command.arguments;
        } else if (m_require) {
            job->timeout->stop();
            QMetaObject::invokeMethod(this, [this, key] {
                finish(key, std::nullopt, QStringLiteral("the sandbox is required and not available"));
            }, Qt::QueuedConnection);
            return;
        }
    }
    job->timeout->start(m_timeoutMs);
    job->process->start(program, args);
}

void SceneCheckRunner::finish(const QString &key, const std::optional<SceneCheckVerdict> &verdict, const QString &failure)
{
    Job *job = m_jobs.take(key);
    if (!job)
        return; // errorOccurred and finished can both fire
    const QString itemDir = job->itemDir;
    job->timeout->stop();
    job->process->disconnect(this);
    job->timeout->disconnect(this);
    if (job->process->state() != QProcess::NotRunning)
        job->process->kill(); // reaped by QProcess's destructor, which runs from deleteLater()
    job->process->deleteLater();
    job->timeout->deleteLater();
    delete job;
    if (verdict)
        m_cache->storeVerdict(itemDir, key, *verdict);
    else
        m_cache->storeFailure(itemDir, key, failure);
    emit verdictReady(itemDir);
    startNext();
}

SceneEngineChoice chooseSceneEngine(const SceneEngineRequest &request)
{
    SceneEngineChoice choice;
    const auto vulkan = [&]() {
        choice.program = request.vulkanProgram;
        choice.vulkan = true;
        choice.label = QStringLiteral("Vulkan");
        return choice;
    };
    // The Vulkan engine runs every scene it can; one it said it cannot fully draw is named as incomplete.
    const auto vulkanIncomplete = [&](const QString &why) {
        vulkan();
        choice.label = QStringLiteral("Vulkan (incomplete: %1)").arg(why);
        return choice;
    };
    if (request.span) {
        // Whatever the setting says: only the Vulkan engine can draw a part of a canvas.
        const auto refuse = [&](const QString &why) {
            choice.program.clear();
            choice.vulkan = false;
            choice.refusal = QStringLiteral("Spanning needs the Vulkan scene engine: %1").arg(why);
            choice.label = QStringLiteral("none (%1)").arg(why);
            return choice;
        };
        if (request.vulkanProgram.isEmpty())
            return refuse(QStringLiteral("murasaki-scene-vk is not installed"));
        choice.key = sceneCheckKey(request.itemDir, request.vulkanProgram, request.assetsDir);
        if (request.cache && request.cache->crashes(choice.key) >= kVulkanCrashLimit)
            return refuse(QStringLiteral("it crashed on this item"));
        if (request.cache) {
            if (const auto verdict = request.cache->verdict(choice.key); verdict && !verdict->ok)
                return refuse(QStringLiteral("it cannot draw this scene (%1)").arg(firstReason(*verdict)));
        }
        return vulkan(); // an unchecked scene is tried: the engine says what it cannot do
    }
    if (request.vulkanProgram.isEmpty()) {
        choice.label = QStringLiteral("none (murasaki-scene-vk is not installed)");
        return choice; // no program: the item says the scene engine is not installed
    }

    choice.key = sceneCheckKey(request.itemDir, request.vulkanProgram, request.assetsDir);
    if (request.cache && request.cache->crashes(choice.key) >= kVulkanCrashLimit) {
        return vulkanIncomplete(QStringLiteral("it crashed on this item"));
    }
    if (request.mode == SceneEngineMode::Vulkan)
        return vulkan();

    // auto: a cached verdict, a recent failure to get one, or a check in the background
    if (request.cache) {
        if (const auto verdict = request.cache->verdict(choice.key)) {
            if (verdict->ok)
                return vulkan();
            return vulkanIncomplete(firstReason(*verdict));
        }
        if (const auto failure = request.cache->failure(choice.key)) {
            return vulkan();
        }
    }
    // nothing else to run, but the check still says whether the scene is drawn in full (the label follows when it is back)
    if (request.runner && request.cache)
        request.runner->request(request.itemDir, request.assetsDir, request.vulkanProgram, choice.key);
    return vulkan();
}

} // namespace murasaki
