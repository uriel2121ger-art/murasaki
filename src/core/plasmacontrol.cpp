#include "plasmacontrol.h"

#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace murasaki::plasma {
namespace {

constexpr int kStateVersion = 1;
const QString kDefaultPresenter = QStringLiteral("auto");

QString xdgDir(const char *variable, const QString &fallbackUnderHome)
{
    const QString value = qEnvironmentVariable(variable);
    return value.isEmpty() ? QDir::homePath() + u'/' + fallbackUnderHome : value;
}

QJsonObject readState(const Paths &paths, bool *exists = nullptr)
{
    const JsonFileRead read = readJsonObjectFile(paths.stateFile());
    if (exists)
        *exists = read.exists && read.problem.isEmpty();
    return read.problem.isEmpty() ? read.root : QJsonObject();
}

Result fail(const QString &message)
{
    Result result;
    result.error = message;
    return result;
}

// Runs the query and parses it.
bool queryDesktops(Backend &backend, QList<Desktop> *desktops, QString *error)
{
    QString output;
    if (!backend.evaluate(queryScript(), &output, error))
        return false;
    if (!parseDesktops(output, desktops)) {
        *error = u"plasmashell answered something unexpected: %1"_s.arg(output.trimmed().left(200));
        return false;
    }
    return true;
}

QString presenterLine(const QString &presenter, bool ok, const QString &error)
{
    return ok ? u"daemon presenter: %1"_s.arg(presenter)
              : u"warning: could not set the daemon presenter to %1 (%2)"_s.arg(presenter, error);
}

} // namespace

QString Paths::stateFile() const { return stateDir + u"/plasma-previous.json"_s; }

QString Paths::backupFile() const
{
    return stateDir + u'/' + QFileInfo(appletsrc).fileName() + u".bak"_s;
}

Paths defaultPaths()
{
    Paths paths;
    const QString config = xdgDir("XDG_CONFIG_HOME", u".config"_s);
    paths.appletsrc = config + u"/plasma-org.kde.plasma.desktop-appletsrc"_s;
    paths.stateDir = xdgDir("XDG_STATE_HOME", u".local/state"_s) + u"/murasaki"_s;
    paths.dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    return paths;
}

QString queryScript()
{
    return u"var r = []; for (const d of desktops()) { r.push({id: String(d.id), plugin: String(d.wallpaperPlugin)}); } "
           "print(JSON.stringify(r));"_s;
}

QString switchScript(const QMap<QString, QString> &pluginById)
{
    QJsonObject map;
    for (auto it = pluginById.cbegin(); it != pluginById.cend(); ++it)
        map.insert(it.key(), it.value());
    // JSON is a JavaScript expression, so the (escaped) object literal is safe to embed.
    return u"var map = %1; for (const d of desktops()) { const p = map[String(d.id)]; if (p !== undefined) { "
           "d.wallpaperPlugin = p; d.reloadConfig(); } }"_s
        .arg(QString::fromUtf8(QJsonDocument(map).toJson(QJsonDocument::Compact)));
}

bool parseDesktops(const QString &output, QList<Desktop> *desktops)
{
    // A script's print() ends in a newline; anything before the JSON (a stray message) is not accepted.
    const QJsonDocument doc = QJsonDocument::fromJson(output.trimmed().toUtf8());
    if (!doc.isArray())
        return false;
    QList<Desktop> parsed;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject object = value.toObject();
        if (object.value(u"id"_s).toString().isEmpty())
            return false;
        parsed.append({object.value(u"id"_s).toString(), object.value(u"plugin"_s).toString()});
    }
    *desktops = parsed;
    return true;
}

bool isPlasmaSession()
{
    const QStringList desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(u':', Qt::SkipEmptyParts);
    for (const QString &desktop : desktops) {
        if (desktop.compare(u"KDE"_s, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool isInstalled(const Paths &paths)
{
    for (const QString &dir : paths.dataDirs) {
        if (QFileInfo::exists(dir + u"/plasma/wallpapers/"_s + kPluginId + u"/metadata.json"_s))
            return true;
    }
    return false;
}

Result enable(Backend &backend, const Options &options)
{
    const Paths &paths = options.paths;
    if (!isInstalled(paths))
        return fail(u"the Plasma package %1 is not installed (cmake --install puts it under share/plasma/wallpapers)"_s
                        .arg(kPluginId));
    QList<Desktop> desktops;
    QString error;
    if (!queryDesktops(backend, &desktops, &error))
        return fail(error);
    if (desktops.isEmpty())
        return fail(u"plasmashell has no desktop containments"_s);

    bool hadState = false;
    const QJsonObject oldState = readState(paths, &hadState);
    QJsonObject previous = oldState.value(u"previous"_s).toObject();
    QMap<QString, QString> toSwitch;
    Result result;
    for (const Desktop &desktop : std::as_const(desktops)) {
        // A desktop already on our plugin keeps what was recorded for it; any other plugin is what to give back.
        if (desktop.plugin != kPluginId || !previous.contains(desktop.id))
            previous.insert(desktop.id, desktop.plugin == kPluginId ? kDefaultPlugin : desktop.plugin);
        if (desktop.plugin != kPluginId)
            toSwitch.insert(desktop.id, kPluginId);
        if (desktop.plugin != kPluginId)
            result.lines << u"desktop %1: %2 -> %3"_s.arg(desktop.id, desktop.plugin, kPluginId);
    }
    const QString script = switchScript(toSwitch);

    if (options.dryRun) {
        if (toSwitch.isEmpty()) {
            result.lines << u"nothing to change: every desktop already uses %1"_s.arg(kPluginId);
            result.ok = true;
            result.active = true;
            return result;
        }
        result.lines << u"dry run, nothing is changed."_s << u"state file: %1"_s.arg(paths.stateFile())
                     << u"config backup: %1 (from %2)"_s.arg(paths.backupFile(), paths.appletsrc)
                     << u"plasmashell script:"_s << script << u"then the daemon presenter is set to plasma"_s;
        result.ok = true;
        return result;
    }

    // The way back is on disk before anything changes.
    QString backup;
    if (!hadState || !QFileInfo::exists(paths.backupFile())) {
        if (QFileInfo::exists(paths.appletsrc)) {
            QDir().mkpath(paths.stateDir);
            QFile::remove(paths.backupFile());
            if (QFile::copy(paths.appletsrc, paths.backupFile()))
                backup = paths.backupFile();
            else
                result.lines << u"warning: could not back up %1"_s.arg(paths.appletsrc);
        }
    }
    // The presenter to give back: the first recording wins, like the plugins.
    QString previousPresenter = oldState.value(u"presenter"_s).toString();
    if (previousPresenter.isEmpty()) {
        QString presenterError;
        if (!backend.presenter(&previousPresenter, &presenterError) || previousPresenter.isEmpty()
            || previousPresenter == u"plasma"_s)
            previousPresenter = kDefaultPresenter;
    }
    QJsonObject state{{u"version"_s, kStateVersion}, {u"previous"_s, previous}, {u"presenter"_s, previousPresenter}};
    state.insert(u"backup"_s, backup.isEmpty() ? oldState.value(u"backup"_s).toString() : backup);
    if (!writeJsonObjectFile(paths.stateFile(), state))
        return fail(u"cannot write %1"_s.arg(paths.stateFile()));
    const QString restoreHint = u" (run `murasakictl plasma disable` to restore)"_s;

    if (!toSwitch.isEmpty()) {
        QString output;
        if (!backend.evaluate(script, &output, &error))
            return fail(error + restoreHint);
        // Read it back: a script that silently did nothing must not look like success.
        QList<Desktop> after;
        if (!queryDesktops(backend, &after, &error))
            return fail(error + restoreHint);
        for (const Desktop &desktop : std::as_const(after)) {
            if (toSwitch.contains(desktop.id) && desktop.plugin != kPluginId)
                return fail(u"plasmashell did not switch desktop %1 (it still uses %2)"_s.arg(desktop.id, desktop.plugin)
                            + restoreHint);
        }
    }
    QString presenterError;
    const bool presenterOk = backend.setPresenter(u"plasma"_s, &presenterError);
    result.lines << presenterLine(u"plasma"_s, presenterOk, presenterError);
    result.ok = true;
    result.active = true;
    return result;
}

Result disable(Backend &backend, const Options &options)
{
    const Paths &paths = options.paths;
    QList<Desktop> desktops;
    QString error;
    if (!queryDesktops(backend, &desktops, &error))
        return fail(error);

    const QJsonObject recorded = readState(paths);
    const QJsonObject previous = recorded.value(u"previous"_s).toObject();
    QString presenter = recorded.value(u"presenter"_s).toString();
    if (presenter.isEmpty() || presenter == u"plasma"_s)
        presenter = kDefaultPresenter;
    QMap<QString, QString> toSwitch;
    Result result;
    for (const Desktop &desktop : std::as_const(desktops)) {
        if (desktop.plugin != kPluginId)
            continue; // not ours (the user picked something else, or never enabled): leave it
        QString target = previous.value(desktop.id).toString();
        if (target.isEmpty() || target == kPluginId)
            target = kDefaultPlugin;
        toSwitch.insert(desktop.id, target);
        result.lines << u"desktop %1: %2 -> %3"_s.arg(desktop.id, desktop.plugin, target);
    }
    if (toSwitch.isEmpty())
        result.lines << u"no desktop uses %1"_s.arg(kPluginId);

    if (options.dryRun) {
        result.lines << u"dry run, nothing is changed."_s;
        if (!toSwitch.isEmpty())
            result.lines << u"plasmashell script:"_s << switchScript(toSwitch);
        result.lines << u"then the daemon presenter is set to %1 and %2 is removed"_s.arg(presenter, paths.stateFile());
        result.ok = true;
        return result;
    }

    if (!toSwitch.isEmpty()) {
        QString output;
        if (!backend.evaluate(switchScript(toSwitch), &output, &error))
            return fail(error);
        QList<Desktop> after;
        if (!queryDesktops(backend, &after, &error))
            return fail(error);
        for (const Desktop &desktop : std::as_const(after)) {
            if (toSwitch.contains(desktop.id) && desktop.plugin == kPluginId)
                return fail(u"plasmashell did not switch desktop %1 back"_s.arg(desktop.id));
        }
    }
    QString presenterError;
    const bool presenterOk = backend.setPresenter(presenter, &presenterError);
    result.lines << presenterLine(presenter, presenterOk, presenterError);
    // The recording is spent; the config backup stays as a safety net.
    QFile::remove(paths.stateFile());
    result.ok = true;
    result.active = false;
    return result;
}

Result status(Backend &backend, const Options &options)
{
    QList<Desktop> desktops;
    QString error;
    if (!queryDesktops(backend, &desktops, &error))
        return fail(error);
    Result result;
    int ours = 0;
    for (const Desktop &desktop : std::as_const(desktops)) {
        const bool isOurs = desktop.plugin == kPluginId;
        ours += isOurs;
        result.lines << u"desktop %1\t%2\t%3"_s.arg(desktop.id, desktop.plugin, isOurs ? u"murasaki"_s : u"other"_s);
    }
    result.lines << u"package installed: %1"_s.arg(isInstalled(options.paths) ? u"yes"_s : u"no"_s);
    result.lines << u"active: %1"_s.arg(ours == 0 ? u"no"_s : ours == desktops.size() ? u"yes"_s : u"partly"_s);
    result.ok = true;
    result.active = ours > 0;
    return result;
}

} // namespace murasaki::plasma
