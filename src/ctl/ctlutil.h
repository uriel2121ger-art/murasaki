#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <QStringList>

#include <QList>

#include <optional>
#include <utility>

namespace murasaki {

// Maps a --desktop argument to a desktop id: an exact id, a 1-based number, or a name (exact, then ignoring case).
// Anything else is returned unchanged, so desktops that do not exist yet can still be assigned.
QString resolveDesktopArg(const QString &arg, const QJsonArray &desktops);

// Whether resolveDesktopArg would match arg to one of the desktops (by id, number or name).
bool desktopArgMatches(const QString &arg, const QJsonArray &desktops);

// The entry of the daemon's Assignments() with exactly this key, or an empty object.
QJsonObject findAssignment(const QJsonArray &assignments, const QString &output, const QString &desktop);

// SetAssignment options that reproduce entry (fill, span, properties) with only muted changed.
QJsonObject optionsWithMuted(const QJsonObject &entry, bool muted);

// One `murasakictl status` line from an entry of the surface's Outputs(): name, desktop, item, state.
QString formatOutputLine(const QJsonObject &output);

// Parses a finite number argument such as --x; false for anything else (empty, trailing text, inf, nan).
bool parseNumberArg(const QString &text, double *value);

// AddPet options from the --x, --y, --width and --fps arguments; an empty text leaves that key out. Returns false
// and sets *error when one is not a number (the daemon checks the ranges and that --fps is a whole number).
bool petAddOptions(const QString &x, const QString &y, const QString &width, const QString &fps, QJsonObject *options,
                   QString *error);

// The values of the behaviour options of `murasakictl pets set`; an option that was not given has no value.
struct PetBehaviourArgs {
    std::optional<QString> clickThrough, lock, keepAbove, hoverHide, focusFade, smartDodge, rotation, flipH, flipV; // nothing: not given; "" is given and wrong
};

// UpdatePet changes from those options: "on" / "off" for the switches (clickThrough, locked, keepAbove, hoverHide, focusFade, smartDodge, flipH,
// flipV) and a number of degrees for --rotation. False with *error naming the option for anything else (the daemon wraps the angle and refuses
// the rest). Setting --hover-hide or --focus-fade on switches the other off, which the daemon does.
bool petBehaviourChanges(const PetBehaviourArgs &args, QJsonObject *changes, QString *error);

// The UpdatePet changes for `murasakictl pets timing <pet id> <words>`: "file" (the file's own delays), "fixed" or
// "fixed <fps>" (every frame the same time; 24, the default, when no number is given; Anima Engine's own rate is 15) and
// "speed <factor>" (the file's delays at that multiple of their speed). False with *error for anything else; the
// daemon checks the ranges.
bool petTimingChanges(const QStringList &words, QJsonObject *changes, QString *error);

// The UpdatePet changes {"stage": {...}} for `murasakictl pets stage <pet id> off|output|desktop [--fit contain|cover|stretch] [--over-fullscreen
// inherit|show|hide]`: the mode is required, the other two are left out when not given (an empty text is given and wrong). False with *error for a word
// the daemon would refuse too, so that a typo exits 2 without a call.
bool petStageChanges(const QString &mode, const std::optional<QString> &fit, const std::optional<QString> &overFullscreen, QJsonObject *changes,
                     QString *error);

// The UpdatePet changes for `murasakictl pets state <pet id> <words>`: `<state> variant <name>`, `<state> sequence <a,b,c> [--order linear|random] [--advance auto|click]
// [--once] [--hold-ms N]`, `clear` (every state) or `clear <state>`; <state> is idle, grabbed, moving or released. The result is checked with the daemon's own rules
// (at most 16 entries, hold 500 to 60000), so that a typo exits 2 without a call. An option that was not given is left out (a sequence has its defaults).
bool petStateChanges(const QStringList &words, const std::optional<QString> &order, const std::optional<QString> &advance, bool once, const std::optional<QString> &holdMs,
                     QJsonObject *changes, QString *error);

// The UpdatePet changes {"tracking": {...}} for `murasakictl pets track <pet id> off|look|follow [--distance N] [--speed X] [--return|--no-return]`: only what was said
// (the rest keeps its value). `returnHome` is true for --return, false for --no-return, left out for neither (both: an error).
bool petTrackChanges(const QString &mode, const std::optional<QString> &distance, const std::optional<QString> &speed, bool returnHome, bool noReturnHome, QJsonObject *changes,
                     QString *error);

// The values of the sound options of `murasakictl pets audio set`; an option that was not given has no value.
struct PetAudioArgs {
    std::optional<QString> asset, mode, volume, offset;
};

// The UpdatePet changes {"audio": {...}} from those options: the asset (64 hex), the mode (none, once, animation_loop, continuous or click), the
// volume (a whole number) and the offset in milliseconds (a whole number). False with *error naming the option for anything else; the daemon checks
// the ranges. Empty `changes` when no option was given.
bool petAudioChanges(const PetAudioArgs &args, QJsonObject *changes, QString *error);

// One `murasakictl pets audio list` line from an entry of ListPetAudioAssets(): asset, name, duration in ms, uses, tab separated.
QString formatPetAudioAssetLine(const QJsonObject &asset);
// The options of `murasakictl pets import sheet|images|zip|gif`; one that was not given has no value ("" is given and wrong).
struct ImportArgs {
    QString kind;
    QStringList sources;
    std::optional<QString> maxSide, maxFps, startSeconds, durationSeconds, autoScene, frame, count, start, padding, spacing, columns, rows, frameMs, fps, size, format, quality, keyColor, tolerance, softness, name;
    bool keepSource = false;
    bool keepAudio = false;
};

// The job description (docs/pets/import.md) for the daemon from those options: --frame WxH, --count N, --start N, --padding L,T, --spacing H,V, --columns N,
// --rows N, --frame-ms N, --fps N, --size first|fit|scale, --format apng|webp, --quality 1-100, --key-color RRGGBB (a leading # is fine), --tolerance 0-100,
// --softness 0-100, --name TEXT, --auto-scene N (mdaa: the scene a pet of the package starts in); for a video --max-side N, --max-fps N, --start S and --duration S (seconds, decimals allowed), --keep-audio. Sources are made absolute. False with *error naming the option for a wrong value; the daemon checks the rest.
bool importSpecJson(const ImportArgs &args, QByteArray *json, QString *error);

// One `murasakictl pets import state` line from a job of ImportState(): id, state, percent, name, then the item id (done) or the error name, tab separated.
QString formatImportLine(const QJsonObject &job);

// One `murasakictl pets list` line from an entry of ListPets(): petId, itemId, variant, output, x, y, width,
// opacity, fps (0: not a fixed rate), "paused" or "playing" and the speed multiplier, tab separated.
QString formatPetLine(const QJsonObject &pet);

// The lines of `murasakictl steamcmd status` from the daemon's SteamCmdStatus(): the state (and account), then one line
// per download: item id, state, percent and error, tab separated.
QStringList formatSteamCmdStatus(const QJsonObject &status);

// The app id `--app` names: 431960, 3474900, or the words wallpaper and pets/anima. Empty for anything else.
QString steamCmdAppArg(const QString &arg);

// What `pets arrange on|off|toggle` should set, given the current state; nullopt for any other argument.
std::optional<bool> arrangeTarget(const QString &arg, bool current);

// The id of the playlist arg names, from the daemon's ListPlaylists(): an exact id, else a name (exact, then ignoring
// case). Empty with *error set when none matches or the name is ambiguous.
QString resolvePlaylistArg(const QString &arg, const QJsonArray &playlists, QString *error);

// One `murasakictl playlist list` line from an entry of ListPlaylists(): id, name, order, interval in minutes and
// number of items, tab separated.
QString formatPlaylistLine(const QJsonObject &playlist);

// UpdatePlaylist changes from key=value arguments: name, order (sequential or shuffle), interval (or
// intervalMinutes) and changeOnLogin (true/false/on/off/yes/no). Returns nullopt and sets *error when there is none,
// or one is malformed or unknown (the daemon checks the ranges).
std::optional<QJsonObject> playlistChanges(const QStringList &pairs, QString *error);

// The (output, desktop) keys of Assignments() that play a playlist. A non-empty output or desktop keeps only the
// entries that apply to it: that exact value, or the wildcard.
QList<std::pair<QString, QString>> playlistTargets(const QJsonArray &assignments, const QString &output,
                                                   const QString &desktop);

// --- Automation (docs/pets/automation.md)

// Text from a file or an import with its control characters replaced by '?', for printing.
QString printable(const QString &text);

// The id an argument names among profiles, triggers or timelines (the daemon's List... JSON): an exact id, else a name (exact, then ignoring case). Empty with *error set
// when none matches or the name is ambiguous (the error lists the ids).
QString resolveNamedArg(const QString &arg, const QJsonArray &entries, const QString &noun, QString *error);

// One `profile list` line from an entry of ListProfiles(): id, name, number of pets, tab separated; `active` marks the one applied last with a star.
QString formatProfileLine(const QJsonObject &profile, bool active);
// One `triggers list` line from an entry of ListTriggers(): id, "on"/"off", name, source kind, action type, tab separated.
QString formatTriggerLine(const QJsonObject &trigger);
// One `timeline list` line from an entry of ListTimelines(): id, name, tracks, speed, tab separated.
QString formatTimelineLine(const QJsonObject &timeline);
// One line per trigger of TriggerState(): id, enabled, available, last outcome, tab separated.
QString formatTriggerStateLine(const QJsonObject &state);

// SetTrigger changes from `triggers add <name> <source kind> <action type> [word=value]...`. A word is source.KEY=VALUE, action.KEY=VALUE or one of cooldownMs, mode and
// enabled; the numbers the model knows (everyMinutes, minutes, index, cooldownMs) are numbers, `days` is a comma list of numbers, enabled is on/off/true/false. False with
// *error naming the word for anything malformed (the daemon checks the rest and refuses an unknown kind or type by name).
bool triggerChangesFromArgs(const QString &name, const QString &sourceKind, const QString &actionType, const QStringList &words, QJsonObject *changes, QString *error);

// `murasakictl pets puppet list`: one line for an item of ListItems/GetItem that is a puppet (format "puppet"): id, status, backend, title, "checked" or "unchecked", the
// canvas, the counts of motion groups and expressions, and the first reason or degraded name when there is one. Tab separated; an empty string for any other item.
QString formatPuppetItemLine(const QJsonObject &item);

// `murasakictl pets puppet status`: one line for an entry of the overlay's Windows() that has a "puppet" object: pet id, state, fps, crashes, the motion group,
// the expression, "look" when the gaze follows the cursor, and the degraded names joined by ", ". Tab separated; an empty string for a pet that is not a puppet.
QString formatPuppetWindowLine(const QJsonObject &window);

} // namespace murasaki
