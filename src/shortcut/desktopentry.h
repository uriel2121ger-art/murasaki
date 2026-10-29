#pragma once

// Desktop Entry files (*.desktop) as the host reads them for the application picker: bounded, from [Desktop Entry] only, as the
// Desktop Entry Specification says (ids, localised names, OnlyShowIn, Hidden and NoDisplay, TryExec). Any local program can write a
// .desktop, so size, count and every string are limited. docs/design/specs/2026-10-21-user-shortcuts-design.md

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace murasaki::shortcut {

inline constexpr qint64 kMaxDesktopFileBytes = 64 * 1024;
inline constexpr int kMaxDesktopFiles = 4096;   // files looked at in one scan
inline constexpr int kMaxDesktopEntries = 1024; // entries kept
inline constexpr int kMaxFieldBytes = 256;      // Name, Icon, TryExec as UTF-8
inline constexpr int kMaxExecBytes = 4096;      // the Exec line the last-resort launcher reads

struct DesktopEntry {
    QString id;       // the desktop file id ("org.mozilla.firefox.desktop")
    QString path;     // the file the entry was read from (its real path)
    QString name;     // localised, cleaned
    QString icon;     // Icon=: a theme name or an absolute path, as written
    QString exec;     // Exec=, as written; read only by the last-resort launcher
    QString windowClass; // StartupWMClass=, or the id without ".desktop": the class its windows carry (what a trigger matches)
    QStringList classAliases; // other names its windows are known by: the id stem, the last reverse-DNS part, the program in Exec=, steam_app_N
    bool terminal = false;
};

// What decides whether an entry is shown here.
struct DesktopEnvironment {
    QStringList currentDesktops;                         // XDG_CURRENT_DESKTOP split on ':'
    QStringList locales;                                 // most specific first: "es_MX", "es"
    std::function<bool(const QString &)> hasExecutable;  // TryExec: whether a program exists (a name in PATH or an absolute path)
};

// The environment of this process.
DesktopEnvironment systemDesktopEnvironment();

// Parses one file's bytes. Nullopt when it is not a launchable application for this environment: not UTF-8, no
// [Desktop Entry] group, Type other than Application, Hidden or NoDisplay, OnlyShowIn / NotShowIn excluding this desktop, a TryExec
// that is not found, no Name or no Exec. `id` is only copied into the result.
std::optional<DesktopEntry> parseDesktopEntry(const QByteArray &data, const QString &id, const DesktopEnvironment &env);

// The roots to scan: $XDG_DATA_HOME/applications, each $XDG_DATA_DIRS/applications, and the Flatpak and Snap exports.
QStringList applicationRoots();

// Scans roots in order (the first root with an id wins), one directory level deep ("a/b.desktop" has the id "a-b.desktop"), and
// returns the entries sorted by name. Limits: kMaxDesktopFiles files looked at, kMaxDesktopEntries kept, kMaxDesktopFileBytes each.
std::vector<DesktopEntry> scanApplications(const QStringList &roots, const DesktopEnvironment &env);

// The entry with this id in the roots, or nullopt (uninstalled, hidden, not for this desktop).
std::optional<DesktopEntry> findApplication(const QStringList &roots, const QString &id, const DesktopEnvironment &env);

} // namespace murasaki::shortcut
