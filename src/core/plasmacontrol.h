#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace murasaki::plasma {

// The wallpaper plugin id of the Plasma package (data/plasma/<id>/).
inline const QString kPluginId = QStringLiteral("io.github.uriel2121ger_art.Murasaki");
// What a desktop gets back when nothing better is known.
inline const QString kDefaultPlugin = QStringLiteral("org.kde.image");

struct Desktop {
    QString id;     // the containment id as plasmashell's scripting reports it
    QString plugin; // its wallpaperPlugin
    bool operator==(const Desktop &) const = default;
};

// What the switch needs from the outside world. The real one talks to plasmashell and murasakid over the session
// bus (DBusBackend); tests use a fake.
class Backend
{
public:
    virtual ~Backend() = default;
    // org.kde.PlasmaShell.evaluateScript; on success `output` holds what the script printed.
    virtual bool evaluate(const QString &script, QString *output, QString *error) = 0;
    // The daemon's `presenter` setting, read and written.
    virtual bool presenter(QString *value, QString *error) = 0;
    virtual bool setPresenter(const QString &presenter, QString *error) = 0;
};

// Where things live. The defaults come from the XDG environment.
struct Paths {
    QString appletsrc;       // Plasma's desktop layout: plasma-org.kde.plasma.desktop-appletsrc
    QString stateDir;        // $XDG_STATE_HOME/murasaki
    QStringList dataDirs;    // where plasma/wallpapers/<id>/metadata.json is looked for
    QString stateFile() const;  // plasma-previous.json
    QString backupFile() const; // a copy of appletsrc taken before the first switch
};
Paths defaultPaths();

// ---- scripts (plasmashell scripting; JavaScript) ----------------------------------------------------------

// Prints the desktop containments as JSON: [{"id":"1","plugin":"org.kde.image"}, ...]. Read only.
QString queryScript();
// Sets the wallpaper plugin of the desktops named in `pluginById` and reloads their configuration.
QString switchScript(const QMap<QString, QString> &pluginById);
// Parses the output of queryScript(); false when it is not that JSON.
bool parseDesktops(const QString &output, QList<Desktop> *desktops);

// ---- operations ---------------------------------------------------------------------------------------------

struct Options {
    bool dryRun = false; // print what would run, change nothing (the read-only query still runs)
    Paths paths = defaultPaths();
};

struct Result {
    bool ok = false;
    QStringList lines; // for the user, one per line
    QString error;     // set when !ok
    bool active = false; // after the operation: some desktop shows the Murasaki wallpaper
};

// Whether XDG_CURRENT_DESKTOP names KDE (it is a colon separated list, e.g. "KDE" or "ubuntu:KDE").
bool isPlasmaSession();

// Whether the package is installed in one of paths.dataDirs.
bool isInstalled(const Paths &paths);

// Records every desktop's current plugin (state file, config backup), switches them to the Murasaki plugin and sets
// presenter=plasma. Running it again keeps the first recording.
Result enable(Backend &backend, const Options &options);
// Gives back the recorded plugins (kDefaultPlugin for unknown ones) to the desktops that still use ours and sets
// presenter=auto. A desktop the user moved to another plugin meanwhile is left alone.
Result disable(Backend &backend, const Options &options);
// One line per desktop: id, plugin and whether it is ours.
Result status(Backend &backend, const Options &options);

} // namespace murasaki::plasma
