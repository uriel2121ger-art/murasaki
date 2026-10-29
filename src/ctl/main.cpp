#include "assignment.h"
#include "autostart.h"
#include "buildinfo.h"
#include "constants.h"
#include "demos.h"
#include "timeline.h"
#include "ctlutil.h"
#include "petcheck.h"
#include "sceneenginelookup.h"
#include "plasmacontrol.h"
#include "plasmadbus.h"
#include "playlist.h"
#include "screenctl.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QDBusVariant>
#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>
#include <QThread>

#include <termios.h>
#include <unistd.h>

#include <optional>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

enum ExitCode { Success = 0, Failure = 1, UsageError = 2 };

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

struct Endpoint {
    QString service;
    QString path;
    QString interface;
    QString displayName;
};

const Endpoint kDaemon{kDBusService, kDBusPath, kDBusInterface, u"murasakid"_s};
const Endpoint kOverlay{kOverlayDBusService, kOverlayDBusPath, kOverlayDBusInterface, u"murasaki-overlay"_s};
const Endpoint kSurface{kSurfaceDBusService, kSurfaceDBusPath, kSurfaceDBusInterface, u"murasaki-surface"_s};

// Returns the first reply argument, or nullopt after printing the error (unless quiet).
std::optional<QVariant> call(const Endpoint &endpoint, const QString &method, const QVariantList &args = {},
                             bool quiet = false)
{
    QDBusMessage message = QDBusMessage::createMethodCall(endpoint.service, endpoint.path, endpoint.interface, method);
    message.setArguments(args);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (!quiet) {
            if (reply.errorName() == u"org.freedesktop.DBus.Error.ServiceUnknown")
                err() << "murasakictl: " << endpoint.displayName << " is not running\n";
            else
                err() << "murasakictl: " << printable(reply.errorMessage()) << '\n';
        }
        return std::nullopt;
    }
    return reply.arguments().value(0);
}

QJsonArray jsonArray(const QVariant &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8()).array();
}

QJsonObject jsonObject(const QVariant &value)
{
    return QJsonDocument::fromJson(value.toString().toUtf8()).object();
}

// The surface knows the compositor's desktops; without it the argument is used as a raw id.
QString desktopKey(const QString &arg)
{
    if (arg.isEmpty() || arg == kWildcard)
        return kWildcard;
    const auto reply = call(kSurface, u"Desktops"_s, {}, true);
    const QJsonArray desktops = reply ? jsonArray(*reply) : QJsonArray();
    if (!desktops.isEmpty() && !desktopArgMatches(arg, desktops))
        err() << "murasakictl: no desktop matches \"" << arg << "\"; using it as a desktop id\n";
    return resolveDesktopArg(arg, desktops);
}


// A secret typed at a terminal does not echo; piped in, it is the next line of stdin.
QString readSecret(const QString &prompt)
{
    err() << prompt;
    err().flush();
    termios saved{};
    const bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    if (tty) {
        termios quiet = saved;
        quiet.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    static QTextStream in(stdin); // one stream: a second one would miss what the first buffered from a pipe
    QString line = in.readLine();
    if (line.endsWith(u'\r'))
        line.chop(1);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        err() << '\n';
    }
    return line;
}

// Imports a sound file through the daemon and waits for the job (the helper decodes it in a sandbox, at most five minutes): the id of the sound.
std::optional<QString> importSound(const QString &pathOrItem, bool fromItem = false)
{
    const auto job = fromItem ? call(kDaemon, u"ImportItemAudio"_s, {pathOrItem}) : call(kDaemon, u"ImportPetAudio"_s, {QFileInfo(pathOrItem).absoluteFilePath()});
    if (!job)
        return std::nullopt;
    for (int i = 0; i < 1800; ++i) {
        const auto state = call(kDaemon, u"PetAudioImportState"_s, {job->toString()}, true);
        const QJsonObject object = state ? QJsonDocument::fromJson(state->toString().toUtf8()).object() : QJsonObject();
        const QString text = object.value(u"state"_s).toString();
        if (text == u"done"_s)
            return object.value(u"result"_s).toObject().value(u"asset"_s).toString();
        if (text == u"failed"_s || text == u"cancelled"_s || object.isEmpty()) {
            err() << "murasakictl: " << (object.value(u"error"_s).toString().isEmpty() ? text : object.value(u"error"_s).toString()) << '\n';
            return std::nullopt;
        }
        QThread::msleep(200);
    }
    err() << "murasakictl: the sound is still being imported; check 'pets audio list' later\n";
    return std::nullopt;
}

// The daemon's revision must be at least `needed` (`what` names the feature in the message); true when it cannot be read, since the call itself
// then reports why the daemon is not there.
bool requireRevision(uint needed, const char *what)
{
    QDBusMessage get = QDBusMessage::createMethodCall(kDaemon.service, kDaemon.path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    get.setArguments({kDaemon.interface, u"ApiVersion"_s});
    const QDBusMessage reply = QDBusConnection::sessionBus().call(get);
    if (reply.type() == QDBusMessage::ErrorMessage)
        return true; // the call itself reports why the daemon is not there
    const uint version = reply.arguments().value(0).value<QDBusVariant>().variant().toUInt();
    if (version >= needed)
        return true;
    err() << "murasakictl: the daemon speaks API " << version << "; " << what << " needs " << needed << "\n";
    return false;
}

// `on`, `off` or `toggle` for a session flag of the daemon (a property and its setter): prints the new value.
bool setSessionFlag(const QString &property, const QString &method, const QString &arg)
{
    bool current = false;
    if (arg == u"toggle") {
        QDBusMessage get = QDBusMessage::createMethodCall(kDaemon.service, kDaemon.path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get.setArguments({kDaemon.interface, property});
        const QDBusMessage reply = QDBusConnection::sessionBus().call(get);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            err() << "murasakictl: " << reply.errorMessage() << '\n';
            return false;
        }
        current = reply.arguments().value(0).value<QDBusVariant>().variant().toBool();
    }
    const bool target = *arrangeTarget(arg, current);
    if (!call(kDaemon, method, {target}))
        return false;
    out() << (target ? u"on"_s : u"off"_s) << '\n';
    return true;
}

// Speed, and a fixed rate above 60, arrived with revision 11; an older daemon would drop the key without a word.
bool requireTimingRevision()
{
    return requireRevision(kTimingDBusApiVersion, "the timing of a pet");
}

QJsonObject steamCmdStatus()
{
    const auto status = call(kDaemon, u"SteamCmdStatus"_s, {}, true);
    return status ? QJsonDocument::fromJson(status->toString().toUtf8()).object() : QJsonObject();
}

int steamCmdCommand(const QCommandLineParser &parser, const QStringList &args, const QCommandLineOption &appOption)
{
    const QString verb = args.at(1);
    if (verb == u"status" && args.size() == 2) {
        const auto status = call(kDaemon, u"SteamCmdStatus"_s);
        if (!status)
            return Failure;
        for (const QString &line : formatSteamCmdStatus(QJsonDocument::fromJson(status->toString().toUtf8()).object()))
            out() << line << '\n';
        return Success;
    }
    if (verb == u"logout" && args.size() == 2)
        return call(kDaemon, u"SteamCmdLogout"_s) ? Success : Failure;
    if (verb == u"cancel" && args.size() <= 3)
        return call(kDaemon, u"SteamCmdCancel"_s, {args.value(2)}) ? Success : Failure;
    if (verb == u"download" && args.size() >= 3) {
        const QString app = steamCmdAppArg(parser.value(appOption));
        if (app.isEmpty()) {
            err() << "murasakictl: --app must be wallpaper, pets or an app id of Wallpaper Engine or Anima Engine\n";
            return UsageError;
        }
        int code = Success;
        for (const QString &id : args.mid(2)) {
            if (!call(kDaemon, u"SteamCmdDownload"_s, {app, id}))
                code = Failure;
        }
        return code;
    }
    if (verb == u"login" && args.size() == 3) {
        if (!call(kDaemon, u"SteamCmdLogin"_s, {args.at(2)}))
            return Failure;
        bool told = false;
        // The daemon drives SteamCMD's prompts; this answers them as they come. Three minutes, like the daemon's own limit.
        for (int waited = 0; waited < 190000; waited += 250) {
            const QJsonObject status = steamCmdStatus();
            const QString state = status.value(u"state"_s).toString();
            const QString prompt = status.value(u"prompt"_s).toString();
            if (state == u"logged-in") {
                out() << "signed in as " << status.value(u"user"_s).toString() << '\n';
                return Success;
            }
            if (state == u"logged-out") {
                err() << "murasakictl: " << status.value(u"message"_s).toString() << '\n';
                return Failure;
            }
            if (state == u"unavailable") {
                err() << "murasakictl: SteamCMD is not installed\n";
                return Failure;
            }
            if (prompt == u"password") {
                if (!call(kDaemon, u"SteamCmdSubmit"_s, {u"password"_s, readSecret(u"Steam password for %1: "_s.arg(args.at(2)))})) {
                    call(kDaemon, u"SteamCmdCancelLogin"_s, {}, true);
                    return Failure;
                }
            } else if (prompt == u"guard") {
                if (!call(kDaemon, u"SteamCmdSubmit"_s, {u"guard"_s, readSecret(u"Steam Guard code: "_s)})) {
                    call(kDaemon, u"SteamCmdCancelLogin"_s, {}, true);
                    return Failure;
                }
            } else if (prompt == u"confirm" && !told) {
                err() << "Confirm the sign-in in the Steam Mobile app on your phone...\n";
                told = true;
            }
            QThread::msleep(250);
        }
        call(kDaemon, u"SteamCmdCancelLogin"_s, {}, true);
        err() << "murasakictl: the sign-in took too long\n";
        return Failure;
    }
    err() << "murasakictl: unknown steamcmd command; see --help\n";
    return UsageError;
}

// The playlist id an argument names (an id or a name), asking the daemon for the list; empty after printing why.
QString playlistId(const QString &arg)
{
    const auto lists = call(kDaemon, u"ListPlaylists"_s);
    if (!lists)
        return {};
    QString problem;
    const QString id = resolvePlaylistArg(arg, jsonArray(*lists), &problem);
    if (id.isEmpty())
        err() << "murasakictl: " << problem << '\n';
    return id;
}

int playlistCommand(const QCommandLineParser &parser, const QStringList &args, const QString &output,
                    const QCommandLineOption &desktopOption, const QCommandLineOption &outputOption,
                    const QCommandLineOption &fillOption, const QCommandLineOption &muteOption,
                    const QCommandLineOption &spanOption)
{
    const QString verb = args.at(1);
    if (verb == u"list" && args.size() == 2) {
        const auto lists = call(kDaemon, u"ListPlaylists"_s);
        if (!lists)
            return Failure;
        for (const QJsonValue &value : jsonArray(*lists))
            out() << formatPlaylistLine(value.toObject()) << '\n';
        return Success;
    }
    if (verb == u"create" && args.size() == 3) {
        const auto id = call(kDaemon, u"CreatePlaylist"_s, {args.at(2)});
        if (!id)
            return Failure;
        out() << id->toString() << '\n';
        return Success;
    }
    if (verb == u"show" && args.size() == 3) {
        const auto lists = call(kDaemon, u"ListPlaylists"_s);
        if (!lists)
            return Failure;
        QString problem;
        const QString id = resolvePlaylistArg(args.at(2), jsonArray(*lists), &problem);
        if (id.isEmpty()) {
            err() << "murasakictl: " << problem << '\n';
            return Failure;
        }
        for (const QJsonValue &value : jsonArray(*lists)) {
            if (value.toObject().value(u"id"_s).toString() != id)
                continue;
            for (const QJsonValue &item : value.toObject().value(u"items"_s).toArray())
                out() << item.toString() << '\n';
        }
        return Success;
    }
    if ((verb == u"add" || verb == u"remove") && args.size() >= 4) {
        const QString id = playlistId(args.at(2));
        if (id.isEmpty())
            return Failure;
        return call(kDaemon, verb == u"add" ? u"AddToPlaylist"_s : u"RemoveFromPlaylist"_s,
                    {id, QVariant(args.mid(3))})
            ? Success
            : Failure;
    }
    if (verb == u"set" && args.size() >= 3) {
        QString problem;
        const auto changes = playlistChanges(args.mid(3), &problem);
        if (!changes) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        const QString id = playlistId(args.at(2));
        if (id.isEmpty())
            return Failure;
        const QString json = QString::fromUtf8(QJsonDocument(*changes).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"UpdatePlaylist"_s, {id, json}) ? Success : Failure;
    }
    if (verb == u"delete" && args.size() == 3) {
        const QString id = playlistId(args.at(2));
        return !id.isEmpty() && call(kDaemon, u"DeletePlaylist"_s, {id}) ? Success : Failure;
    }
    if (verb == u"apply" && args.size() == 3) {
        const QString id = playlistId(args.at(2));
        if (id.isEmpty())
            return Failure;
        QJsonObject options;
        if (parser.isSet(fillOption))
            options.insert(u"fill"_s, parser.value(fillOption));
        if (parser.isSet(muteOption))
            options.insert(u"muted"_s, true);
        if (parser.isSet(spanOption))
            options.insert(u"span"_s, true);
        const QString desktop = desktopKey(parser.value(desktopOption));
        const QString json = QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"SetAssignment"_s, {output, desktop, playlistItemId(id), json}) ? Success : Failure;
    }
    if ((verb == u"next" || verb == u"prev") && args.size() == 2) {
        const auto all = call(kDaemon, u"Assignments"_s);
        if (!all)
            return Failure;
        const QString desktopArg = parser.isSet(desktopOption) ? desktopKey(parser.value(desktopOption)) : QString();
        const auto targets = playlistTargets(jsonArray(*all), parser.isSet(outputOption) ? output : QString(), desktopArg);
        if (targets.isEmpty()) {
            err() << "murasakictl: no playlist is playing there\n";
            return Failure;
        }
        int code = Success;
        for (const auto &[targetOutput, targetDesktop] : targets) {
            if (!call(kDaemon, verb == u"next" ? u"NextInPlaylist"_s : u"PreviousInPlaylist"_s,
                      {targetOutput, targetDesktop}))
                code = Failure;
        }
        return code;
    }
    err() << "murasakictl: unknown playlist command; see --help\n";
    return UsageError;
}
// The id an argument names among the daemon's List<noun> answer (an id or a name); empty after printing why.
QString namedId(const QString &method, const QString &arg, const QString &noun)
{
    const auto list = call(kDaemon, method);
    if (!list)
        return {};
    QString problem;
    const QString id = resolveNamedArg(arg, jsonArray(*list), noun, &problem);
    if (id.isEmpty())
        err() << "murasakictl: " << problem << '\n';
    return id;
}

int profileCommand(const QStringList &args)
{
    if (!requireRevision(kAutomationDBusApiVersion, "profiles"))
        return Failure;
    const QString verb = args.at(1);
    if (verb == u"list" && args.size() == 2) {
        const auto lists = call(kDaemon, u"ListProfiles"_s);
        if (!lists)
            return Failure;
        QString active;
        QDBusMessage get = QDBusMessage::createMethodCall(kDaemon.service, kDaemon.path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get.setArguments({kDaemon.interface, u"ActiveProfile"_s});
        const QDBusMessage reply = QDBusConnection::sessionBus().call(get);
        if (reply.type() != QDBusMessage::ErrorMessage)
            active = reply.arguments().value(0).value<QDBusVariant>().variant().toString();
        for (const QJsonValue &value : jsonArray(*lists))
            out() << formatProfileLine(value.toObject(), !active.isEmpty() && value.toObject().value(u"id"_s).toString() == active) << '\n';
        return Success;
    }
    if (verb == u"save" && args.size() == 3) {
        const auto id = call(kDaemon, u"SaveProfile"_s, {args.at(2)});
        if (!id)
            return Failure;
        out() << id->toString() << '\n';
        return Success;
    }
    if (verb == u"switch" && args.size() == 3) {
        const auto report = call(kDaemon, u"SwitchProfile"_s, {args.at(2)});
        if (!report)
            return Failure;
        const QJsonObject r = QJsonDocument::fromJson(report->toString().toUtf8()).object();
        for (const QJsonValue &skipped : r.value(u"skipped"_s).toArray())
            err() << "murasakictl: skipped a pet of " << printable(skipped.toObject().value(u"itemId"_s).toString()) << ": " << printable(skipped.toObject().value(u"reason"_s).toString()) << '\n';
        out() << "kept " << r.value(u"kept"_s).toInt() << ", updated " << r.value(u"updated"_s).toInt() << ", added " << r.value(u"added"_s).toInt() << ", removed "
              << r.value(u"removed"_s).toInt() << '\n';
        return Success;
    }
    if (verb == u"rename" && args.size() == 4)
        return call(kDaemon, u"RenameProfile"_s, {args.at(2), args.at(3)}) ? Success : Failure;
    if (verb == u"delete" && args.size() == 3)
        return call(kDaemon, u"DeleteProfile"_s, {args.at(2)}) ? Success : Failure;
    err() << "murasakictl: profile list | save <name> | switch <name> | rename <old> <new> | delete <name>\n";
    return UsageError;
}

// The daemon's ScreenWatchState (a property of JSON), or an empty object after printing why not.
QJsonObject screenWatchState()
{
    QDBusMessage get = QDBusMessage::createMethodCall(kDaemon.service, kDaemon.path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    get.setArguments({kDaemon.interface, u"ScreenWatchState"_s});
    const QDBusMessage reply = QDBusConnection::sessionBus().call(get);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        err() << "murasakictl: " << printable(reply.errorMessage()) << '\n';
        return {};
    }
    return QJsonDocument::fromJson(reply.arguments().value(0).value<QDBusVariant>().variant().toString().toUtf8()).object();
}

// The pictures of a capture are kept only when the user says so: shown (as files, with --save-to) and confirmed (--yes, or a y at the terminal).
// 1: keep them; 0: the user said no; -1: nobody to ask (a usage error: run it again with --yes).
int confirmKeeping(int count, bool yes, bool saved)
{
    if (yes)
        return 1;
    if (!isatty(STDIN_FILENO)) {
        err() << "murasakictl: nothing was kept; run it again with --yes to keep " << count << " picture(s)" << (saved ? " (look at them first: they are in the folder of --save-to)" : " (add --save-to DIR to look at them first)") << '\n';
        return -1;
    }
    err() << "Keep " << count << " picture(s) as the reference? [y/N] ";
    err().flush();
    static QTextStream in(stdin);
    const QString answer = in.readLine().trimmed().toLower();
    return answer == u"y" || answer == u"yes" ? 1 : 0;
}

// murasakictl triggers screen start | stop | status | regions <trigger> | capture <trigger> <region> [--steps N] [--save-to DIR] [--yes]
int screenCommand(const QCommandLineParser &parser, const QStringList &args, const QCommandLineOption &stepsOption, const QCommandLineOption &saveToOption, const QCommandLineOption &yesOption)
{
    if (!requireRevision(kScreenDBusApiVersion, "screen triggers"))
        return Failure;
    const QString verb = args.value(2);
    if (verb != u"capture" && (parser.isSet(stepsOption) || parser.isSet(saveToOption) || parser.isSet(yesOption))) {
        err() << "murasakictl: --steps, --save-to and --yes are for \"triggers screen capture\" only\n";
        return UsageError;
    }
    if (verb == u"start" && args.size() == 3) {
        if (!call(kDaemon, u"StartScreenWatch"_s))
            return Failure;
        for (const QString &line : formatScreenWatchState(screenWatchState()))
            out() << line << '\n';
        out() << "the desktop asks you which screens to share; answer its dialog\n";
        return Success;
    }
    if (verb == u"stop" && args.size() == 3)
        return call(kDaemon, u"StopScreenWatch"_s) ? Success : Failure;
    if (verb == u"status" && args.size() == 3) {
        const QJsonObject state = screenWatchState();
        if (state.isEmpty())
            return Failure;
        for (const QString &line : formatScreenWatchState(state))
            out() << line << '\n';
        return Success;
    }
    if ((verb == u"regions" && args.size() == 4) || (verb == u"capture" && args.size() == 5)) {
        const auto list = call(kDaemon, u"ListTriggers"_s);
        if (!list)
            return Failure;
        QString problem;
        const QString id = resolveNamedArg(args.at(3), jsonArray(*list), u"trigger"_s, &problem);
        if (id.isEmpty()) {
            err() << "murasakictl: " << problem << '\n';
            return Failure;
        }
        QJsonObject trigger;
        for (const QJsonValue &v : jsonArray(*list))
            if (v.toObject().value(u"id"_s).toString() == id)
                trigger = v.toObject();
        if (trigger.value(u"source"_s).toObject().value(u"kind"_s).toString() != u"screen") {
            err() << "murasakictl: \"" << printable(trigger.value(u"name"_s).toString()) << "\" is not a screen trigger\n";
            return Failure;
        }
        if (verb == u"regions") {
            QJsonObject live;
            if (const auto state = call(kDaemon, u"TriggerState"_s, {}, true)) {
                for (const QJsonValue &s : jsonObject(*state).value(u"states"_s).toArray())
                    if (s.toObject().value(u"id"_s).toString() == id) {
                        // The score is under "screen" (only while watching); the reference counts sit on the state itself.
                        live = s.toObject().value(u"screen"_s).toObject();
                        live.insert(u"refs"_s, s.toObject().value(u"refs"_s));
                    }
            }
            for (const QString &line : formatScreenRegions(trigger, live))
                out() << line << '\n';
            return Success;
        }
        bool numberOk = false;
        const int region = args.at(4).toInt(&numberOk);
        const int regionCount = trigger.value(u"source"_s).toObject().value(u"data"_s).toObject().value(u"regions"_s).toArray().size();
        if (!numberOk || region < 0 || region > 63) {
            err() << "murasakictl: the region is a number from 0\n";
            return UsageError;
        }
        if (region >= regionCount) {
            err() << "murasakictl: the trigger has " << regionCount << " region(s); the regions count from 0\n";
            return UsageError;
        }
        int steps = 1;
        if (parser.isSet(stepsOption) && !parseScreenSteps(parser.value(stepsOption), &steps, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        ScreenCaptureWaiter waiter(QDBusConnection::sessionBus(), kDaemon.service, kDaemon.path, kDaemon.interface);
        const auto request = call(kDaemon, u"CaptureScreenReference"_s, {id, region, steps});
        if (!request)
            return Failure;
        const auto result = waiter.wait(request->toString(), 30000 + steps * 2000);
        if (!result) {
            err() << "murasakictl: the daemon sent no picture; is the screen being watched? (triggers screen start)\n";
            return Failure;
        }
        // What the daemon holds for this request is dropped whenever it is not kept (it would forget it after a minute anyway).
        const auto discard = [&] { call(kDaemon, u"DiscardScreenReference"_s, {request->toString()}, true); };
        const auto capture = parseScreenCapture(*result, &problem);
        if (!capture) {
            err() << "murasakictl: " << problem << '\n';
            discard();
            return Failure;
        }
        for (int i = 0; i < capture->pngs.size(); ++i)
            out() << "picture " << i << ": " << capture->sizes.at(i).width() << 'x' << capture->sizes.at(i).height() << '\n';
        bool saved = false;
        if (parser.isSet(saveToOption)) {
            const QString dir = parser.value(saveToOption);
            if (!QDir().mkpath(dir)) {
                err() << "murasakictl: cannot make the folder " << printable(dir) << '\n';
                discard();
                return Failure;
            }
            // What an earlier run left (step0.png ... stepN.png) would be taken for part of this one.
            const QStringList stale = QDir(dir).entryList({u"step*.png"_s}, QDir::Files);
            for (const QString &name : stale)
                if (QRegularExpression(u"\\Astep[0-9]+\\.png\\z"_s).match(name).hasMatch())
                    QFile::remove(QDir(dir).filePath(name));
            for (int i = 0; i < capture->pngs.size(); ++i) {
                QFile file(QDir(dir).filePath(u"step%1.png"_s.arg(i)));
                if (!file.open(QIODevice::WriteOnly) || file.write(capture->pngs.at(i)) != capture->pngs.at(i).size()) {
                    err() << "murasakictl: cannot write " << printable(file.fileName()) << '\n';
                    discard();
                    return Failure;
                }
            }
            saved = true;
            out() << "saved to " << printable(dir) << '\n';
        }
        const int keep = confirmKeeping(int(capture->pngs.size()), parser.isSet(yesOption), saved);
        if (keep != 1) {
            discard();
            return keep < 0 ? UsageError : Failure;
        }
        if (!call(kDaemon, u"CommitScreenReference"_s, {request->toString()}))
            return Failure;
        out() << "kept " << capture->pngs.size() << " picture(s) for region " << region << '\n';
        return Success;
    }
    err() << "murasakictl: triggers screen start | stop | status | regions <trigger> | capture <trigger> <region> [--steps N] [--save-to DIR] [--yes]\n";
    return UsageError;
}

int triggersCommand(const QStringList &args)
{
    if (!requireRevision(kAutomationDBusApiVersion, "triggers"))
        return Failure;
    const QString verb = args.at(1);
    if (verb == u"list" && args.size() == 2) {
        const auto list = call(kDaemon, u"ListTriggers"_s);
        if (!list)
            return Failure;
        for (const QJsonValue &value : jsonArray(*list))
            out() << formatTriggerLine(value.toObject()) << '\n';
        return Success;
    }
    if (verb == u"add" && args.size() >= 5) {
        QJsonObject changes;
        QString problem;
        if (!triggerChangesFromArgs(args.at(2), args.at(3), args.at(4), args.mid(5), &changes, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        const auto id = call(kDaemon, u"SetTrigger"_s, {QString(), QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact))});
        if (!id)
            return Failure;
        out() << id->toString() << '\n';
        return Success;
    }
    if ((verb == u"enable" || verb == u"disable" || verb == u"remove" || verb == u"test") && args.size() == 3) {
        const QString id = namedId(u"ListTriggers"_s, args.at(2), u"trigger"_s);
        if (id.isEmpty())
            return Failure;
        if (verb == u"remove")
            return call(kDaemon, u"DeleteTrigger"_s, {id}) ? Success : Failure;
        if (verb == u"test") {
            const auto ran = call(kDaemon, u"TestTrigger"_s, {id});
            if (!ran)
                return Failure;
            out() << (ran->toBool() ? "ran" : "did not run") << '\n';
            return ran->toBool() ? Success : Failure;
        }
        return call(kDaemon, u"SetTriggerEnabled"_s, {id, verb == u"enable"}) ? Success : Failure;
    }
    if (verb == u"fire" && args.size() == 3) {
        const auto matched = call(kDaemon, u"FireTrigger"_s, {args.at(2)});
        if (!matched)
            return Failure;
        if (!matched->toBool())
            err() << "murasakictl: no trigger is called \"" << args.at(2) << "\"\n";
        return matched->toBool() ? Success : Failure;
    }
    if (verb == u"state" && args.size() == 2) {
        const auto state = call(kDaemon, u"TriggerState"_s);
        if (!state)
            return Failure;
        const QJsonObject object = QJsonDocument::fromJson(state->toString().toUtf8()).object();
        for (const QJsonValue &value : object.value(u"states"_s).toArray())
            out() << formatTriggerStateLine(value.toObject()) << '\n';
        return Success;
    }
    err() << "murasakictl: triggers list | add <name> <source kind> <action type> [source.KEY=V] [action.KEY=V] [cooldownMs=N] [mode=hold] | enable|disable|remove|test <trigger> | fire <name> | state | screen ...\n";
    return UsageError;
}

int timelineCommand(const QStringList &args)
{
    if (!requireRevision(kAutomationDBusApiVersion, "timelines"))
        return Failure;
    const QString verb = args.at(1);
    if (verb == u"list" && args.size() == 2) {
        const auto list = call(kDaemon, u"ListTimelines"_s);
        if (!list)
            return Failure;
        for (const QJsonValue &value : jsonArray(*list))
            out() << formatTimelineLine(value.toObject()) << '\n';
        return Success;
    }
    if (verb == u"show" && args.size() == 3) {
        const auto list = call(kDaemon, u"ListTimelines"_s);
        if (!list)
            return Failure;
        QString problem;
        const QString id = resolveNamedArg(args.at(2), jsonArray(*list), u"timeline"_s, &problem);
        if (id.isEmpty()) {
            err() << "murasakictl: " << problem << '\n';
            return Failure;
        }
        for (const QJsonValue &value : jsonArray(*list))
            if (value.toObject().value(u"id"_s).toString() == id)
                out() << QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented);
        return Success;
    }
    if ((verb == u"run" || verb == u"stop" || verb == u"delete") && args.size() == 3) {
        const QString id = namedId(u"ListTimelines"_s, args.at(2), u"timeline"_s);
        if (id.isEmpty())
            return Failure;
        return call(kDaemon, verb == u"run" ? u"RunTimeline"_s : verb == u"stop" ? u"StopTimeline"_s : u"DeleteTimeline"_s, {id}) ? Success : Failure;
    }
    if (verb == u"from-scene" && args.size() == 4) {
        bool ok = false;
        const int scene = args.at(3).toInt(&ok);
        if (!ok || scene < 1) {
            err() << "murasakictl: the scene is a number from 1\n";
            return UsageError;
        }
        const auto id = call(kDaemon, u"TimelineFromScene"_s, {args.at(2), scene - 1});
        if (!id)
            return Failure;
        out() << id->toString() << '\n';
        return Success;
    }
    if (verb == u"from-pack" && args.size() == 3) {
        const auto made = call(kDaemon, u"ImportPackAutomation"_s, {args.at(2)});
        if (!made)
            return Failure;
        const QJsonObject r = QJsonDocument::fromJson(made->toString().toUtf8()).object();
        out() << r.value(u"timeline"_s).toString() << '\n';
        for (const QJsonValue &note : r.value(u"notes"_s).toArray())
            err() << "murasakictl: " << printable(note.toString()) << '\n';
        err() << "murasakictl: the triggers it made are disabled; enable one with: triggers enable <name>\n";
        return Success;
    }
    if (verb == u"set" && args.size() == 4) {
        // timeline set <name> <file|->: the timeline's JSON (docs/pets/automation.md) from a file or stdin; it makes the timeline or replaces the one of that name.
        QByteArray data;
        if (args.at(3) == u"-") {
            QFile in;
            if (in.open(stdin, QIODevice::ReadOnly))
                data = in.read(1 << 20);
        } else {
            QFile file(args.at(3));
            if (!file.open(QIODevice::ReadOnly)) {
                err() << "murasakictl: cannot read " << args.at(3) << '\n';
                return UsageError;
            }
            data = file.read(1 << 20);
        }
        QJsonParseError parseError;
        QJsonObject object = QJsonDocument::fromJson(data, &parseError).object();
        if (parseError.error != QJsonParseError::NoError || object.isEmpty()) {
            err() << "murasakictl: the timeline is not a JSON object\n";
            return UsageError;
        }
        object.insert(u"name"_s, args.at(2));
        QString existing;
        if (const auto list = call(kDaemon, u"ListTimelines"_s)) {
            QString ignored;
            existing = resolveNamedArg(args.at(2), jsonArray(*list), u"timeline"_s, &ignored);
        }
        const auto id = call(kDaemon, u"SetTimeline"_s, {existing, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))});
        if (!id)
            return Failure;
        out() << id->toString() << '\n';
        return Success;
    }
    err() << "murasakictl: timeline list | show <name> | run <name> | stop <name> | delete <name> | set <name> <file|-> | from-scene <item> <n> | from-pack <item>\n";
    return UsageError;
}

// ---------------------------------------------------------------- the examples ("Try it", src/core/demos.h): the same calls as the GUI's buttons

// A timeline made from the pets (or the one an earlier run made with the same name, replaced): its id, empty after printing why.
QString saveDemoTimeline(const QJsonObject &spec, QString *savedName = nullptr)
{
    const auto list = call(kDaemon, u"ListTimelines"_s);
    if (!list)
        return {};
    QJsonObject named = spec;
    named.insert(u"name"_s, demoFreeName(jsonArray(*list), spec.value(u"name"_s).toString()));
    if (savedName)
        *savedName = named.value(u"name"_s).toString();
    const auto id = call(kDaemon, u"SetTimeline"_s, {demoIdByName(jsonArray(*list), named.value(u"name"_s).toString()), QString::fromUtf8(QJsonDocument(named).toJson(QJsonDocument::Compact))});
    return id ? id->toString() : QString();
}

int noPetForDemo()
{
    err() << "murasakictl: the examples are made from your pets and there is none on the desktop; add one first (pets add <item id>)\n";
    return Failure;
}

// The screen example: a screen trigger that is OFF, with a region to move and no picture (yours are taken with "triggers screen capture"). One made before is kept as it is.
int demoScreen(const QJsonArray &pets, const DemoNames &names)
{
    if (!requireRevision(kScreenDBusApiVersion, "screen triggers"))
        return Failure;
    if (pets.isEmpty())
        return noPetForDemo();
    const auto list = call(kDaemon, u"ListTriggers"_s);
    if (!list)
        return Failure;
    const QString name = demoFreeName(jsonArray(*list), names.screen);
    QString id = demoIdByName(jsonArray(*list), name);
    const bool existing = !id.isEmpty();
    if (!existing) {
        const auto made = call(kDaemon, u"SetTrigger"_s, {QString(), QString::fromUtf8(QJsonDocument(demoScreenTrigger(name)).toJson(QJsonDocument::Compact))});
        if (!made)
            return Failure;
        id = made->toString();
    }
    out() << "screen: " << (existing ? "the example screen trigger is already there \"" : "made the screen trigger \"") << printable(name) << "\" (off, with a region and no picture); it hides your pets while a picture you capture is on the screen\n";
    out() << "  1. turn on \"Screen triggers\" in the Settings page of the app    (the setting screenTriggersEnabled; it is off until you do)\n"
          << "  2. murasakictl triggers enable \"" << printable(name) << "\"\n"
          << "  3. murasakictl triggers screen start               (the desktop asks which screens to share; answer its dialog)\n"
          << "  4. show what should hide the pets, then: murasakictl triggers screen capture \"" << printable(name) << "\" 0 --save-to DIR --yes\n"
          << "  5. murasakictl triggers screen regions \"" << printable(name) << "\"   (the live score); murasakictl demo remove deletes the example\n";
    return Success;
}

int demoTimelines(const QJsonArray &pets, const QSize &screen, const DemoNames &names, bool trigger)
{
    const auto spec = demoTimeline(pets, screen, names.timeline);
    if (!spec)
        return noPetForDemo();
    QString timelineName;
    const QString timelineId = saveDemoTimeline(*spec, &timelineName);
    if (timelineId.isEmpty())
        return Failure;
    if (!trigger) {
        if (!call(kDaemon, u"RunTimeline"_s, {timelineId}))
            return Failure;
        out() << "timeline: \"" << printable(timelineName) << "\" is playing; the pets go back when it ends (timeline show/run/stop \"" << printable(timelineName) << "\")\n";
        return Success;
    }
    const auto triggers = call(kDaemon, u"ListTriggers"_s);
    if (!triggers)
        return Failure;
    const QString triggerName = demoFreeName(jsonArray(*triggers), names.trigger);
    const auto id = call(kDaemon, u"SetTrigger"_s, {demoIdByName(jsonArray(*triggers), triggerName), QString::fromUtf8(QJsonDocument(demoTrigger(timelineId, triggerName)).toJson(QJsonDocument::Compact))});
    if (!id)
        return Failure;
    out() << "triggers: \"" << printable(triggerName) << "\" plays \"" << printable(timelineName) << "\". Give the shortcut a key in System Settings, Shortcuts, Murasaki"
          << " (the desktop asks the first time), and press it; or run it now: triggers test \"" << printable(triggerName) << "\"\n";
    return Success;
}

int demoProfiles(const QJsonArray &pets, const DemoNames &names)
{
    if (pets.isEmpty())
        return noPetForDemo();
    const auto profiles = call(kDaemon, u"ListProfiles"_s);
    if (!profiles)
        return Failure;
    const QString profileName = demoFreeName(jsonArray(*profiles), names.profile);
    // One call that saves and marks it, and leaves the active profile and the pet slots of the timelines alone.
    if (!call(kDaemon, u"SaveExampleProfile"_s, {profileName}))
        return Failure;
    out() << "profile: \"" << printable(profileName) << "\" saved; nothing else changed. Move or remove a pet and run: profile switch \"" << printable(profileName)
          << "\" to see them come back (switching also points timelines that use a pet slot at that profile's pets; demo remove goes back to the profile you had)\n";
    return Success;
}

int demoTracking(const QJsonArray &pets, const DemoNames &names, const QString &petId)
{
    QJsonObject pet;
    for (const QJsonValue &value : pets)
        if (!petId.isEmpty() && value.toObject().value(u"petId"_s).toString() == petId)
            pet = value.toObject();
    if (pet.isEmpty() && !petId.isEmpty()) {
        err() << "murasakictl: no pet has the id " << printable(petId) << '\n';
        return Failure;
    }
    if (pet.isEmpty())
        pet = demoTrackingPet(pets);
    if (pet.isEmpty())
        return noPetForDemo();
    const QString id = saveDemoTimeline(demoTrackingTimeline(pet, names.tracking));
    if (id.isEmpty() || !call(kDaemon, u"RunTimeline"_s, {id}))
        return Failure;
    out() << "tracking: pet " << printable(pet.value(u"petId"_s).toString()) << (demoTrackingMode(pet) == u"look" ? " (locked or a stage) turns to face the cursor for " : " follows the cursor for ") << kDemoTrackingMs / 1000
          << " seconds; nothing is stored (needs the compositor to tell where the cursor is)\n";
    return Success;
}

// What the examples made, deleted: only what carries the marker (origin "demo"), triggers first, then timelines (stopped first), then profiles.
int demoRemove()
{
    int counts[3] = {0, 0, 0};
    const auto sweep = [&](const QString &listMethod, const QString &deleteMethod, int &count, bool stop) {
        const auto list = call(kDaemon, listMethod);
        if (!list)
            return false;
        for (const QString &id : demoIds(jsonArray(*list))) {
            if (stop)
                call(kDaemon, u"StopTimeline"_s, {id}, true);
            if (!call(kDaemon, deleteMethod, {id}))
                return false;
            ++count;
        }
        return true;
    };
    // (each kind is swept even if another failed)
    const bool okTriggers = sweep(u"ListTriggers"_s, u"DeleteTrigger"_s, counts[0], false);
    const bool okTimelines = sweep(u"ListTimelines"_s, u"DeleteTimeline"_s, counts[1], true);
    const bool okProfiles = sweep(u"ListProfiles"_s, u"DeleteProfile"_s, counts[2], false);
    const bool ok = okTriggers && okTimelines && okProfiles;
    out() << "removed " << counts[0] << " trigger(s), " << counts[1] << " timeline(s), " << counts[2] << " profile(s)\n";
    return ok ? Success : Failure;
}

int demoCommand(const QCommandLineParser &parser, const QStringList &args, const QCommandLineOption &screenOption, const QCommandLineOption &petOption)
{
    if (!requireRevision(kDemosDBusApiVersion, "the examples"))
        return Failure;
    const QString kind = args.value(1, u"all"_s);
    if (args.size() > 2 || !QStringList{u"timelines"_s, u"triggers"_s, u"profiles"_s, u"tracking"_s, u"screen"_s, u"all"_s, u"remove"_s}.contains(kind)) {
        err() << "murasakictl: demo [timelines|triggers|profiles|tracking|screen|all] [--screen WxH] [--pet ID] | demo remove\n";
        return UsageError;
    }
    if ((kind != u"tracking" && kind != u"all" && parser.isSet(petOption)) || (kind != u"timelines" && kind != u"triggers" && kind != u"all" && parser.isSet(screenOption))) {
        err() << "murasakictl: --pet is for demo tracking and --screen for demo timelines and triggers\n";
        return UsageError;
    }
    if (kind == u"remove")
        return demoRemove();
    QSize screen = kDemoDefaultScreen;
    if (parser.isSet(screenOption)) {
        const QStringList parts = parser.value(screenOption).split(u'x');
        bool okWidth = false, okHeight = false;
        const int width = parts.value(0).toInt(&okWidth), height = parts.value(1).toInt(&okHeight);
        if (parts.size() != 2 || !okWidth || !okHeight || width < 200 || height < 200 || width > 16384 || height > 16384) {
            err() << "murasakictl: --screen is WIDTHxHEIGHT, 200 to 16384 (the screen the example is laid out in)\n";
            return UsageError;
        }
        screen = QSize(width, height);
    }
    const auto list = call(kDaemon, u"ListPets"_s);
    if (!list)
        return Failure;
    const QJsonArray pets = jsonArray(*list);
    const DemoNames names = DemoNames::english();
    const QString petId = parser.value(petOption);
    if (kind == u"timelines")
        return demoTimelines(pets, screen, names, false);
    if (kind == u"triggers")
        return demoTimelines(pets, screen, names, true);
    if (kind == u"profiles")
        return demoProfiles(pets, names);
    if (kind == u"tracking")
        return demoTracking(pets, names, petId);
    if (kind == u"screen")
        return demoScreen(pets, names);
    int worst = Success;
    for (const int step : {0, 1, 2, 3}) {
        const int code = step == 0 ? demoTimelines(pets, screen, names, false) : step == 1 ? demoTimelines(pets, screen, names, true) : step == 2 ? demoProfiles(pets, names) : demoTracking(pets, names, petId);
        if (code != Success)
            worst = code;
        if (pets.isEmpty())
            break;
        if (step == 0 && code == Success) {
            // A timeline that ends puts back everything it touched, the follow of the last step included: the last step starts when it is over.
            const auto spec = demoTimeline(pets, screen, names.timeline);
            const auto timeline = spec ? applyTimelineChanges(Timeline{}, *spec) : std::nullopt;
            if (timeline) {
                out() << "demo: waiting for the timeline to end (" << (timeline->lengthMs() + 500) / 1000 << " s)\n";
                out().flush();
                QThread::msleep(ulong(timeline->lengthMs() + 500));
            }
        }
    }
    return worst;
}
} // namespace

// `pets puppet check FOLDER`: the check the daemon runs, run here (murasaki-puppet --check behind murasaki-sandbox --profile puppet when the helper is there). The line it prints is the answer;
// the status is Failure when the folder is refused or the checker could not run.
int runPuppetCheck(const QString &folder)
{
    const QString helper = findPuppetHelper();
    if (helper.isEmpty()) {
        err() << "murasakictl: murasaki-puppet is not installed\n";
        return Failure;
    }
    const QString sandbox = findSandboxHelper();
    // The same rule as the daemon's check: a folder is hostile input, so with importRequireSandbox on (the default; also when the daemon cannot be asked) the parser never runs outside the sandbox.
    bool require = true;
    if (const auto settings = call(kDaemon, u"Settings"_s, {}, true))
        require = jsonObject(*settings).value(u"importRequireSandbox"_s).toBool(true);
    if (sandbox.isEmpty() && require) {
        err() << "murasakictl: sandbox unavailable: murasaki-sandbox is not installed, and importRequireSandbox is on\n";
        return Failure;
    }
    QStringList command;
    if (!sandbox.isEmpty()) {
        command << sandbox << u"--profile"_s << u"puppet"_s << u"--item"_s << folder;
        if (require)
            command << u"--require"_s;
        command << u"--"_s;
    }
    command << helper << u"--check"_s << folder;
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    const QString program = command.takeFirst();
    process.start(program, command);
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        err() << "murasakictl: the check took too long and was stopped\n";
        return Failure;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        err() << "murasakictl: the check failed (" << (process.exitStatus() == QProcess::NormalExit ? u"exit code %1"_s.arg(process.exitCode()) : u"it crashed"_s) << "): "
              << printable(QString::fromUtf8(process.readAllStandardError()).left(300)) << '\n';
        return Failure;
    }
    const QByteArray line = process.readAllStandardOutput().left(65536).split('\n').value(0);
    out() << printable(QString::fromUtf8(line)) << '\n';
    return QJsonDocument::fromJson(line).object().value(u"ok"_s).toBool() ? Success : Failure;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"murasakictl"_s);
    QCoreApplication::setApplicationVersion(versionString());

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Control Murasaki wallpapers.\n\nCommands:\n"
                                     "  list                    library items (id, kind, status, title)\n"
                                     "  set <item id>           assign an item (--output, --desktop, --fill, --mute, --span)\n"
                                     "  unset                   remove an assignment\n"
                                     "  mute on|off             mute or unmute an assignment\n"
                                     "  status                  outputs, desktops and what they show\n"
                                     "  desktops                the compositor's desktops (number, name, id)\n"
                                     "  autostart on|off        start the wallpaper with your session\n"
                                     "  plasma enable|disable|status\n"
                                     "                          show the wallpaper as the Plasma desktop wallpaper\n"
                                     "  pets list               desktop pets (id, item, variant, output, x, y, width, opacity, fps, state)\n"
                                     "  pets add <item id>      show a pet on the desktop; prints its id\n"
                                     "  pets check <item>       can the overlay play it? one JSON line (an item dir, an animation file or anima:<id>)\n"
                                     "  pets puppet add <folder>    make a pet item of a puppet folder (one Inochi2D .inp, or a Cubism model3.json and its files); prints the item id. Needs API 20\n"
                                     "  pets puppet remove <item id>   forget a puppet item (the folder stays); not while a pet shows it\n"
                                     "  pets puppet list        the puppet items: id, status, backend, title, checked, canvas, motion groups, expressions, first reason\n"
                                     "  pets puppet check <folder>   run the puppet check on a folder now, behind the sandbox when there is one; prints its one JSON line\n"
                                     "  pets puppet status      the overlay's view of every puppet pet: state, fps, crashes, motion group, expression, look, what it degrades by\n"
                                     "  pets set <pet id> [--click-through on|off] [--lock on|off] [--keep-above on|off] [--hover-hide on|off]\n"
                                     "                          [--focus-fade on|off] [--smart-dodge on|off] [--rotation DEG] [--flip-h on|off] [--flip-v on|off]\n"
                                     "                          change what a pet does (stored with it; at least one option)\n"
                                     "  pets stage <pet id> off|output|desktop [--fit contain|cover|stretch] [--over-fullscreen inherit|show|hide]\n"
                                     "                          show the pet as a transparent animation across one screen (output) or all of them\n"
                                     "                          (desktop); off puts it back where it was. It never takes the mouse or keyboard\n"
                                     "  pets state <pet id> idle|grabbed|moving|released variant <name> | sequence <a,b,c> [--order linear|random] [--advance auto|click] [--once]\n"
                                     "                          [--hold-ms N]; pets state <pet id> clear [state]. What the pet shows in each state (a variant of its item, or a\n"
                                     "                          sequence of them); a state without one shows what the one before it in\n"
                                     "                          moving, grabbed, idle shows\n"
                                     "  pets track <pet id> off|look|follow [--distance N] [--speed X] [--return|--no-return]\n"
                                     "                          Mouse Tracking: look turns the pet to the cursor, follow walks it after the cursor and\n"
                                     "                          stops N px away (160), at X times 200 px/s; --no-return leaves it where it stopped\n"
                                     "                          when it is turned off. Needs the compositor's global pointer\n"
                                     "  pets timing <pet id> file|fixed [fps]|speed <factor>\n"
                                     "                          how fast it plays: the file's own delays (default), every frame\n"
                                     "                          the same time (24 fps, the default, if none is given), or the file's delays at\n"
                                     "                          0.25 to 4 times their speed\n"
                                     "  pets audio import <file>   turn a sound file (mp3, wav, ogg, flac, m4a, aac, wma; or a video's sound) into a sound a pet can use; prints its id\n"
                                     "  pets audio from-item <item id>   the sound of a pet imported from a video with --keep-audio; prints its id\n"
                                     "  pets audio list|remove <id>|gc   the imported sounds (id, name, ms, pets that use it), remove one, forget the unused\n"
                                     "  pets audio set <pet id> [--asset ID | --file FILE] [--mode none|once|animation_loop|continuous|click]\n"
                                     "                          [--volume 0-100] [--offset MS]   give a pet a sound (off by default; the sound plays by its mode)\n"
                                     "  pets audio off <pet id>    take the sound off a pet\n"
                                     "  pets audio test <pet id>   play a pet's sound once now; pets audio stop silences every pet\n"
                                     "  pets volume <0-100>|mute|unmute   the master volume and mute of every pet's sound\n"
                                     "  pets import sheet|images|zip|gif <file>... [--frame WxH] [--count N] [--start N] [--padding L,T] [--spacing H,V] [--frame-ms N] [--fps N]\n"
                                     "                          [--size first|fit|scale] [--format apng|webp] [--key-color RRGGBB] [--name TEXT] [--keep-source] [--preview] [--no-wait]\n"
                                     "                          make a pet from a sprite sheet, images (files or a folder), a ZIP of frames or a GIF; prints the item id\n"
                                     "  pets import video <clip> [--max-side PX] [--max-fps N] [--start S] [--duration S] [--keep-audio]\n"
                                     "  pets import mdaa <package.mdaa> [--auto-scene N]\n"
                                     "  pets scene <pet id> <n> start scene n (counted from 0) of an imported .mdaa package on a pet\n"
                                     "  pets import state|cancel <job>|remove <item> [--delete-files]\n"
                                     "  pets hide|show|toggle   hide or show every pet (toggle-hidden is the same; not stored: they are shown again after a restart)\n"
                                     "  pets rescue             make every pet visible, solid and grabbable again: click-through off, keep above on,\n"
                                     "                          hover hide, focus fade, smart dodge and lock off, opacity 1 when below 0.5\n"
                                     "  pets remove <pet id>    remove a pet\n"
                                     "  pets arrange on|off|toggle\n"
                                     "                          move and resize pets with the mouse\n"
                                     "  wallpaper interact on|off|toggle\n"
                                     "                          click and drag on interactive wallpapers (web, scenes) under the Plasma desktop;\n"
                                     "                          Esc, the same shortcut (Meta+Shift+W) or 60 s without input end it\n"
                                     "  steamcmd status         the SteamCMD account and downloads\n"
                                     "  steamcmd login <account>   sign in with your own Steam account (asks for the password\n"
                                     "                          and Steam Guard code; never stored)\n"
                                     "  steamcmd logout         sign out and end SteamCMD's session\n"
                                     "  steamcmd download <item id>...   download Workshop items (--app wallpaper|pets)\n"
                                     "  steamcmd cancel [item id]   cancel one download, or all\n"
                                     "  playlist list           playlists (id, name, order, minutes, items)\n"
                                     "  playlist create <name>  make an empty playlist; prints its id\n"
                                     "  playlist show <id>      the items of a playlist\n"
                                     "  playlist add <id> <item id>...\n"
                                     "  playlist remove <id> <item id>...\n"
                                     "  playlist set <id> key=value...\n"
                                     "                          name, order (sequential|shuffle), interval (minutes),\n"
                                     "                          changeOnLogin (on|off)\n"
                                     "  playlist delete <id>    delete a playlist that no assignment plays\n"
                                     "  playlist apply <id>     play a playlist (--output, --desktop, --fill, --mute, --span)\n"
                                     "  playlist next|prev      skip to the next or previous item (--output, --desktop)\n"
                                     "                          <id> may also be the playlist's name\n"
                                     "  profile list|save <name>|switch <name>|rename <old> <new>|delete <name>\n"
                                     "                          save the pets on the desktop under a name and switch to another setup in one step\n"
                                     "  triggers list | add <name> <source kind> <action type> [source.KEY=V] [action.KEY=V] [cooldownMs=N] [mode=hold]\n"
                                     "                          e.g. triggers add nap idle hide-all source.minutes=10; triggers add wake idle show-all source.minutes=10 source.state=back\n"
                                     "  triggers enable|disable|remove|test <trigger> | fire <name> | state\n"
                                     "  triggers screen start|stop|status|regions <trigger>|capture <trigger> <region> [--steps N] [--save-to DIR] [--yes]\n"
                                     "                          screen triggers (they look at regions of your screen when you start them; the desktop asks first, and a picture is kept only when you say so)\n"
                                     "                          make things happen by themselves (a window gets the focus, a time, a signal): a trigger acts on the pets only\n"
                                     "  timeline list|show <name>|run <name>|stop <name>|delete <name>|set <name> <file|->|from-scene <item> <n>|from-pack <item>\n"
                                     "                          timelines: tracks of actions on pets (show, hide, place, move, sound, wait)\n"
                                     "  demo [timelines|triggers|profiles|tracking|screen|all] [--screen WxH] [--pet ID]\n"
                                     "                          Try it: make a small working example out of your own pets (a timeline that plays, a trigger on a key, a profile,\n"
                                     "                          the pet following the cursor for 20 s; screen makes a screen trigger that is off and says how to capture your own picture for it, it is not part of all);\n"
                                     "                          everything it makes is marked, and nothing stored about your pets changes\n"
                                     "  demo remove             delete exactly what demo made (your own timelines, triggers and profiles stay)\n"
                                     "  pets runtime clear [pet id]   put the pets that a timeline or a trigger hid or moved back (the rescue does it too)"_s);
    const QCommandLineOption help = parser.addHelpOption();
    const QCommandLineOption version = parser.addVersionOption();
    const QCommandLineOption outputOption(u"output"_s, u"Output (connector name such as DP-1); default: all."_s,
                                          u"name"_s);
    const QCommandLineOption desktopOption(u"desktop"_s, u"Virtual desktop (number, name or id); default: all."_s,
                                           u"desktop"_s);
    const QCommandLineOption fillOption(u"fill"_s, u"How the wallpaper fits the output: fill, fit or stretch."_s,
                                        u"mode"_s);
    const QCommandLineOption muteOption(u"mute"_s, u"Assign muted."_s);
    const QCommandLineOption spanOption(u"span"_s, u"Draw one wallpaper across all screens as a single picture (not with --output)."_s);
    const QCommandLineOption appOption(u"app"_s, u"steamcmd download: wallpaper (default) or pets (Anima Engine), or an app id."_s, u"app"_s);
    const QCommandLineOption dryRunOption(u"dry-run"_s, u"plasma: print what would run and change nothing."_s);
    const QCommandLineOption variantOption(u"variant"_s, u"pets add: animation name; default: the first."_s, u"name"_s);
    const QCommandLineOption xOption(u"x"_s, u"pets add: left edge in pixels from the output's corner."_s, u"px"_s);
    const QCommandLineOption yOption(u"y"_s, u"pets add: top edge in pixels from the output's corner."_s, u"px"_s);
    const QCommandLineOption widthOption(u"width"_s, u"pets add: displayed width in pixels."_s, u"px"_s);
    const QCommandLineOption fpsOption(u"fps"_s,
                                       u"pets add: show every frame for 1/n second (n from 1 to 120; default 24; Anima Engine's own rate is 15); 0 plays the file's own timing."_s,
                                       u"n"_s);
    const QCommandLineOption speedOption(u"speed"_s,
                                         u"pets add: play the file's delays at this multiple of their speed, 0.25 to 4 (not with --fps); pets track follow: times 200 px/s, 0.1 to 4."_s,
                                         u"factor"_s);
    const QCommandLineOption clickThroughOption(u"click-through"_s, u"pets set: on lets the pointer through the pet, off makes it take the pointer on its opaque pixels."_s, u"on|off"_s);
    const QCommandLineOption lockOption(u"lock"_s, u"pets set: on keeps a drag from moving the pet."_s, u"on|off"_s);
    const QCommandLineOption keepAboveOption(u"keep-above"_s, u"pets set: off puts the pet below the windows where the compositor allows it."_s, u"on|off"_s);
    const QCommandLineOption hoverHideOption(u"hover-hide"_s, u"pets set: on hides the pet under the cursor (turns --focus-fade off)."_s, u"on|off"_s);
    const QCommandLineOption focusFadeOption(u"focus-fade"_s, u"pets set: on fades the pet under the cursor and lets it through (turns --hover-hide off)."_s, u"on|off"_s);
    const QCommandLineOption smartDodgeOption(u"smart-dodge"_s, u"pets set: on makes the pet step away from the cursor."_s, u"on|off"_s);
    const QCommandLineOption rotationOption(u"rotation"_s, u"pets set: degrees, wrapped into 0 to 360."_s, u"deg"_s);
    const QCommandLineOption flipHOption(u"flip-h"_s, u"pets set: on mirrors the pet left to right."_s, u"on|off"_s);
    const QCommandLineOption assetOption(u"asset"_s, u"pets audio set: the id of an imported sound."_s, u"id"_s);
    const QCommandLineOption fileOption(u"file"_s, u"pets audio set: a sound file to import and use."_s, u"file"_s);
    const QCommandLineOption modeOption(u"mode"_s, u"pets audio set: none, once, animation_loop, continuous or click."_s, u"mode"_s);
    const QCommandLineOption volumeOption(u"volume"_s, u"pets audio set: 0 to 100 (75 by default)."_s, u"n"_s);
    const QCommandLineOption offsetOption(u"offset"_s, u"pets audio set: -5000 to 5000 ms; positive delays the start, negative starts that far into the sound."_s, u"ms"_s);
    const QCommandLineOption flipVOption(u"flip-v"_s, u"pets set: on mirrors the pet top to bottom."_s, u"on|off"_s);
    parser.addOptions({assetOption, fileOption, modeOption, volumeOption, offsetOption});
    const QCommandLineOption fitOption(u"fit"_s, u"pets stage: contain (default; fitted inside, transparent bands), cover (fills, the excess cropped) or stretch (fills, distorted)."_s, u"fit"_s);
    const QCommandLineOption overFullscreenOption(u"over-fullscreen"_s, u"pets stage: inherit (the setting stageHideOverFullscreen), show or hide while a fullscreen window is on the screen."_s, u"how"_s);
    parser.addOptions({fitOption, overFullscreenOption});
    // pets state, pets track
    const QCommandLineOption orderOption(u"order"_s, u"pets state sequence: linear (in order, then again; default) or random (never the same twice in a row)."_s, u"order"_s);
    const QCommandLineOption advanceOption(u"advance"_s, u"pets state sequence: auto (the next one after its pass, or --hold-ms; default) or click (when the pet is clicked)."_s, u"how"_s);
    const QCommandLineOption onceOption(u"once"_s, u"pets state sequence: each animation plays one pass and its last frame stays."_s);
    const QCommandLineOption holdMsOption(u"hold-ms"_s, u"pets state sequence: how long an animation that loops shows in an automatic sequence, 500 to 60000 (default 4000)."_s, u"ms"_s);
    const QCommandLineOption distanceOption(u"distance"_s, u"pets track follow: the gap kept from the cursor in pixels, 0 to 4000 (default 160)."_s, u"px"_s);
    const QCommandLineOption returnOption(u"return"_s, u"pets track: when following ends, glide back to the stored place (default)."_s);
    const QCommandLineOption noReturnOption(u"no-return"_s, u"pets track: when following ends, stay where the pet is and store that place."_s);
    parser.addOptions({orderOption, advanceOption, onceOption, holdMsOption, distanceOption, returnOption, noReturnOption});
    // pets import
    const QCommandLineOption frameOption(u"frame"_s, u"pets import sheet: frame size, WIDTHxHEIGHT (default 64x64)."_s, u"WxH"_s);
    const QCommandLineOption countOption(u"count"_s, u"pets import sheet: how many frames (default 4)."_s, u"n"_s);
    const QCommandLineOption startOption(u"start"_s, u"pets import sheet: first frame, counted from 1 (default 1)."_s, u"n"_s);
    const QCommandLineOption paddingOption(u"padding"_s, u"pets import sheet: LEFT,TOP margin before the first frame."_s, u"l,t"_s);
    const QCommandLineOption spacingOption(u"spacing"_s, u"pets import sheet: HORIZONTAL,VERTICAL gap between frames."_s, u"h,v"_s);
    const QCommandLineOption columnsOption(u"columns"_s, u"pets import sheet: use at most this many columns."_s, u"n"_s);
    const QCommandLineOption rowsOption(u"rows"_s, u"pets import sheet: use at most this many rows."_s, u"n"_s);
    const QCommandLineOption autoSceneOption(u"auto-scene"_s, u"pets import mdaa: the scene (counted from 0) a pet of the package starts in."_s, u"n"_s);
    const QCommandLineOption maxSideOption(u"max-side"_s, u"pets import video: longest side of the frames, 16 to 8192 (default 1024)."_s, u"px"_s);
    const QCommandLineOption maxFpsOption(u"max-fps"_s, u"pets import video: most frames a second kept, 1 to 60 (default 30)."_s, u"fps"_s);
    const QCommandLineOption durationOption(u"duration"_s, u"pets import video: seconds kept from --start (default to the end)."_s, u"s"_s);
    const QCommandLineOption keepAudioOption(u"keep-audio"_s, u"pets import video: keep the audio (a copy of the video, for later; nothing plays it now)."_s);
    const QCommandLineOption frameMsOption(u"frame-ms"_s, u"pets import: milliseconds every frame is shown."_s, u"ms"_s);
    const QCommandLineOption sizeOption(u"size"_s, u"pets import images|zip: images of other sizes: first (refuse), fit (letterbox, default) or scale."_s, u"policy"_s);
    const QCommandLineOption formatOption(u"format"_s, u"pets import: output file, apng (default) or webp."_s, u"format"_s);
    const QCommandLineOption qualityOption(u"quality"_s, u"pets import --format webp: 1 to 99 lossy, 100 lossless (default)."_s, u"q"_s);
    const QCommandLineOption keyColorOption(u"key-color"_s, u"pets import: make this colour (RRGGBB) transparent."_s, u"rrggbb"_s);
    const QCommandLineOption toleranceOption(u"tolerance"_s, u"pets import --key-color: how far from the colour is still transparent, 0 to 100 (default 50)."_s, u"percent"_s);
    const QCommandLineOption softnessOption(u"softness"_s, u"pets import --key-color: width of the soft edge, 0 to 100 (default 10)."_s, u"percent"_s);
    const QCommandLineOption nameOption(u"name"_s, u"pets import: the pet's name."_s, u"text"_s);
    const QCommandLineOption keepSourceOption(u"keep-source"_s, u"pets import: keep a copy of the source files with the result."_s);
    const QCommandLineOption previewOption(u"preview"_s, u"pets import: print what the import would make (headers only) and stop."_s);
    const QCommandLineOption noWaitOption(u"no-wait"_s, u"pets import: print the job id and return."_s);
    const QCommandLineOption deleteFilesOption(u"delete-files"_s, u"pets import remove: also delete the converted files."_s);
    parser.addOptions({maxSideOption, maxFpsOption, durationOption, keepAudioOption, autoSceneOption, frameOption, countOption, startOption, paddingOption, spacingOption, columnsOption, rowsOption, frameMsOption, sizeOption, formatOption,
                       qualityOption, keyColorOption, toleranceOption, softnessOption, nameOption, keepSourceOption, previewOption, noWaitOption, deleteFilesOption});
    parser.addOptions({clickThroughOption, lockOption, keepAboveOption, hoverHideOption, focusFadeOption, smartDodgeOption, rotationOption, flipHOption, flipVOption});
    const QCommandLineOption screenOption(u"screen"_s, u"demo: the size of the screen the examples are laid out in, WIDTHxHEIGHT (default 1280x720)."_s, u"WxH"_s);
    const QCommandLineOption petOption(u"pet"_s, u"demo tracking: the id of the pet that follows the cursor (default: the first that can walk)."_s, u"id"_s);
    parser.addOptions({screenOption, petOption});
    const QCommandLineOption stepsOption(u"steps"_s, u"triggers screen capture: how many reference pictures to take one after another, 1 to 64 (default 1)."_s, u"n"_s);
    const QCommandLineOption saveToOption(u"save-to"_s, u"triggers screen capture: write the captured pictures (step0.png, ...) to this folder so you can look at them before keeping them."_s, u"dir"_s);
    const QCommandLineOption yesOption(u"yes"_s, u"triggers screen capture: keep the pictures without asking."_s);
    parser.addOptions({stepsOption, saveToOption, yesOption});
    parser.addOptions({outputOption, desktopOption, fillOption, muteOption, spanOption, appOption, dryRunOption, variantOption, xOption,
                       yOption, widthOption, fpsOption, speedOption});
    parser.addPositionalArgument(u"command"_s, u"See the command list above."_s);
    // parse() instead of process(): process() exits with 1 on bad usage, the contract says 2.
    if (!parser.parse(app.arguments())) {
        err() << "murasakictl: " << parser.errorText() << '\n' << parser.helpText();
        return UsageError;
    }
    if (parser.isSet(help))
        parser.showHelp(0);
    if (parser.isSet(version))
        parser.showVersion();

    const QStringList args = parser.positionalArguments();
    const QString command = args.value(0);
    const QString output = parser.isSet(outputOption) ? parser.value(outputOption) : QString(kWildcard);

    if (command == u"list" && args.size() == 1) {
        const auto items = call(kDaemon, u"ListItems"_s);
        if (!items)
            return Failure;
        for (const QJsonValue &value : jsonArray(*items)) {
            const QJsonObject item = value.toObject();
            out() << item.value(u"id"_s).toString() << '\t' << item.value(u"kind"_s).toString() << '\t'
                  << item.value(u"status"_s).toString() << '\t' << item.value(u"title"_s).toString() << '\n';
        }
        return Success;
    }
    if (command == u"set" && args.size() == 2) {
        QJsonObject options;
        if (parser.isSet(fillOption))
            options.insert(u"fill"_s, parser.value(fillOption));
        if (parser.isSet(muteOption))
            options.insert(u"muted"_s, true);
        if (parser.isSet(spanOption))
            options.insert(u"span"_s, true);
        const QString desktop = desktopKey(parser.value(desktopOption));
        const QString json = QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"SetAssignment"_s, {output, desktop, args.at(1), json}) ? Success : Failure;
    }
    if (command == u"unset" && args.size() == 1) {
        const QString desktop = desktopKey(parser.value(desktopOption));
        return call(kDaemon, u"ClearAssignment"_s, {output, desktop}) ? Success : Failure;
    }
    if (command == u"mute" && args.size() == 2 && (args.at(1) == u"on" || args.at(1) == u"off")) {
        const QString desktop = desktopKey(parser.value(desktopOption));
        const auto all = call(kDaemon, u"Assignments"_s);
        if (!all)
            return Failure;
        const QJsonObject entry = findAssignment(jsonArray(*all), output, desktop);
        if (entry.isEmpty()) {
            err() << "murasakictl: nothing is assigned to output " << output << " on desktop " << desktop << '\n';
            return Failure;
        }
        const QJsonObject options = optionsWithMuted(entry, args.at(1) == u"on");
        const QString json = QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"SetAssignment"_s, {output, desktop, entry.value(u"itemId"_s).toString(), json})
            ? Success
            : Failure;
    }
    if (command == u"status" && args.size() == 1) {
        const auto outputs = call(kSurface, u"Outputs"_s);
        if (!outputs)
            return Failure;
        for (const QJsonValue &value : jsonArray(*outputs))
            out() << formatOutputLine(value.toObject()) << '\n';
        return Success;
    }
    if (command == u"desktops" && args.size() == 1) {
        const auto desktops = call(kSurface, u"Desktops"_s);
        if (!desktops)
            return Failure;
        for (const QJsonValue &value : jsonArray(*desktops)) {
            const QJsonObject desktop = value.toObject();
            out() << desktop.value(u"number"_s).toInt() << '\t' << desktop.value(u"name"_s).toString() << '\t'
                  << desktop.value(u"id"_s).toString() << '\n';
        }
        return Success;
    }
    if (command == u"autostart" && args.size() == 2 && (args.at(1) == u"on" || args.at(1) == u"off")) {
        QString error;
        bool couldNotRun = false;
        if (autostart::setEnabled(args.at(1) == u"on", &error, &couldNotRun))
            return Success;
        // systemctl explains its own non-zero exits on stderr; only add a line when it could not run.
        if (couldNotRun)
            err() << "murasakictl: " << error << '\n';
        return Failure;
    }
    if (command == u"plasma" && args.size() == 2
        && (args.at(1) == u"enable" || args.at(1) == u"disable" || args.at(1) == u"status")) {
        plasma::DBusBackend backend;
        plasma::Options options;
        options.dryRun = parser.isSet(dryRunOption);
        const plasma::Result result = args.at(1) == u"enable" ? plasma::enable(backend, options)
            : args.at(1) == u"disable"                         ? plasma::disable(backend, options)
                                                               : plasma::status(backend, options);
        for (const QString &line : result.lines)
            out() << line << '\n';
        if (!result.ok)
            err() << "murasakictl: " << result.error << '\n';
        return result.ok ? Success : Failure;
    }
    if (command == u"pets" && args.size() >= 3 && args.at(1) == u"import") {
        const QString verb = args.at(2);
        if (!requireRevision(kImportDBusApiVersion, "pet imports"))
            return Failure;
        if (verb == u"state" && args.size() == 3) {
            const auto state = call(kDaemon, u"ImportState"_s);
            if (!state)
                return Failure;
            for (const QJsonValue &job : jsonArray(*state))
                out() << formatImportLine(job.toObject()) << '\n';
            return Success;
        }
        if (verb == u"cancel" && args.size() == 4)
            return call(kDaemon, u"CancelImport"_s, {args.at(3)}) ? Success : Failure;
        if (verb == u"remove" && args.size() == 4)
            return call(kDaemon, u"RemoveImportedPet"_s, {args.at(3), parser.isSet(deleteFilesOption)}) ? Success : Failure;
        if ((verb == u"sheet" || verb == u"images" || verb == u"zip" || verb == u"gif" || verb == u"mdaa" || verb == u"video") && args.size() >= 4) {
            ImportArgs importArgs;
            importArgs.kind = verb;
            importArgs.sources = args.mid(3);
            const auto given = [&](const QCommandLineOption &o) { return parser.isSet(o) ? std::optional<QString>(parser.value(o)) : std::nullopt; };
            importArgs.autoScene = given(autoSceneOption);
            importArgs.frame = given(frameOption);
            importArgs.count = given(countOption);
            if (verb == u"video") { // (for a video --start is seconds into the clip, not a first frame)
                importArgs.startSeconds = given(startOption);
                importArgs.maxSide = given(maxSideOption);
                importArgs.maxFps = given(maxFpsOption);
                importArgs.durationSeconds = given(durationOption);
                importArgs.keepAudio = parser.isSet(keepAudioOption);
            } else {
                importArgs.start = given(startOption);
            }
            importArgs.padding = given(paddingOption);
            importArgs.spacing = given(spacingOption);
            importArgs.columns = given(columnsOption);
            importArgs.rows = given(rowsOption);
            importArgs.frameMs = given(frameMsOption);
            importArgs.fps = given(fpsOption);
            importArgs.size = given(sizeOption);
            importArgs.format = given(formatOption);
            importArgs.quality = given(qualityOption);
            importArgs.keyColor = given(keyColorOption);
            importArgs.tolerance = given(toleranceOption);
            importArgs.softness = given(softnessOption);
            importArgs.name = given(nameOption);
            importArgs.keepSource = parser.isSet(keepSourceOption);
            QByteArray spec;
            QString problem;
            if (!importSpecJson(importArgs, &spec, &problem)) {
                err() << "murasakictl: " << problem << '\n';
                return UsageError;
            }
            if (parser.isSet(previewOption)) {
                const auto preview = call(kDaemon, u"PreviewImport"_s, {QString::fromUtf8(spec)});
                if (!preview)
                    return Failure;
                out() << preview->toString() << '\n';
                return QJsonDocument::fromJson(preview->toString().toUtf8()).object().value(u"ok"_s).toBool() ? Success : Failure;
            }
            const auto job = call(kDaemon, u"ImportPet"_s, {QString::fromUtf8(spec)});
            if (!job)
                return Failure;
            const QString jobId = job->toString();
            if (parser.isSet(noWaitOption)) {
                out() << jobId << '\n';
                return Success;
            }
            // Wait for the job: poll the state twice a second. It never waits for ever: a job that is gone from the list (the daemon restarted, or 50 newer
            // jobs pushed it out) or done without a pet ends the wait with an error, and so does 40 minutes without an end.
            int missing = 0, doneWithoutPet = 0;
            for (int polls = 0; polls < 40 * 60 * 2; ++polls) {
                const auto state = call(kDaemon, u"ImportState"_s);
                if (!state)
                    return Failure;
                bool found = false;
                for (const QJsonValue &value : jsonArray(*state)) {
                    const QJsonObject j = value.toObject();
                    if (j.value(u"id"_s).toString() != jobId)
                        continue;
                    found = true;
                    const QString st = j.value(u"state"_s).toString();
                    if (st == u"done"_s && !j.value(u"itemId"_s).toString().isEmpty()) {
                        out() << j.value(u"itemId"_s).toString() << '\n';
                        return Success;
                    }
                    if (st == u"done"_s && ++doneWithoutPet > 10) {
                        err() << "murasakictl: the import finished but the pet was not added\n";
                        return Failure;
                    }
                    if (st == u"failed"_s || st == u"cancelled"_s) {
                        err() << "murasakictl: " << j.value(u"error"_s).toString() << '\n';
                        return Failure;
                    }
                }
                if (!found && ++missing > 4) {
                    err() << "murasakictl: the import is no longer listed (the service restarted?)\n";
                    return Failure;
                }
                QThread::msleep(500);
            }
            err() << "murasakictl: the import is still running after 40 minutes; it keeps going (murasakictl pets import state)\n";
            return Failure;
        }
        err() << parser.helpText();
        return UsageError;
    }
    if (command == u"pets" && args.size() == 4 && args.at(1) == u"scene") {
        bool ok = false;
        const int index = args.at(3).toInt(&ok);
        if (!ok || index < 0) {
            err() << "murasakictl: the scene is a number counted from 0\n";
            return UsageError;
        }
        if (!requireRevision(kImportDBusApiVersion, "pet scenes"))
            return Failure;
        return call(kDaemon, u"StartScene"_s, {args.at(2), index}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() == 3 && args.at(1) == u"check")
        return runPetCheck(args.at(2));
    if (command == u"pets" && args.size() >= 3 && args.at(1) == u"puppet") {
        const QString verb = args.at(2);
        if (verb == u"add" && args.size() == 4) {
            if (!requireRevision(kPuppetsDBusApiVersion, "puppet pets"))
                return Failure;
            const auto id = call(kDaemon, u"AddLocalPuppetItem"_s, {QFileInfo(args.at(3)).absoluteFilePath()});
            if (!id)
                return Failure;
            out() << id->toString() << '\n';
            return Success;
        }
        if (verb == u"remove" && args.size() == 4) {
            if (!requireRevision(kPuppetsDBusApiVersion, "puppet pets"))
                return Failure;
            return call(kDaemon, u"RemoveLocalPuppetItem"_s, {args.at(3)}) ? Success : Failure;
        }
        if (verb == u"list" && args.size() == 3) {
            const auto items = call(kDaemon, u"ListItems"_s);
            if (!items)
                return Failure;
            for (const QJsonValue &value : jsonArray(*items)) {
                const QJsonObject summary = value.toObject();
                if (summary.value(u"format"_s).toString() != u"puppet"_s)
                    continue;
                // (the summary has no puppet object: the full item does)
                const auto full = call(kDaemon, u"GetItem"_s, {summary.value(u"id"_s).toString()});
                const QString line = formatPuppetItemLine(full ? jsonObject(*full) : summary);
                if (!line.isEmpty())
                    out() << line << '\n';
            }
            return Success;
        }
        if (verb == u"check" && args.size() == 4)
            return runPuppetCheck(QFileInfo(args.at(3)).absoluteFilePath());
        if (verb == u"status" && args.size() == 3) {
            const auto windows = call(kOverlay, u"Windows"_s);
            if (!windows)
                return Failure;
            for (const QJsonValue &value : jsonArray(*windows)) {
                const QString line = formatPuppetWindowLine(value.toObject());
                if (!line.isEmpty())
                    out() << line << '\n';
            }
            return Success;
        }
        err() << "murasakictl: pets puppet add|remove|list|check|status (see --help)\n";
        return UsageError;
    }
    if (command == u"pets" && args.size() == 2 && args.at(1) == u"list") {
        const auto pets = call(kDaemon, u"ListPets"_s);
        if (!pets)
            return Failure;
        for (const QJsonValue &value : jsonArray(*pets))
            out() << formatPetLine(value.toObject()) << '\n';
        return Success;
    }
    if (command == u"pets" && args.size() == 3 && args.at(1) == u"add") {
        QJsonObject options;
        QString problem;
        if (!petAddOptions(parser.value(xOption), parser.value(yOption), parser.value(widthOption), parser.value(fpsOption), &options,
                           &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        if (parser.isSet(speedOption) && !requireTimingRevision())
            return Failure;
        if (parser.isSet(speedOption)) {
            double speed = 0;
            if (!parseNumberArg(parser.value(speedOption), &speed)) {
                err() << "murasakictl: --speed needs a number, not \"" << parser.value(speedOption) << "\"\n";
                return UsageError;
            }
            options.insert(u"speed"_s, speed);
        }
        const QString json = QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact));
        const auto petId = call(kDaemon, u"AddPet"_s, {args.at(2), parser.value(variantOption), output, json});
        if (!petId)
            return Failure;
        out() << petId->toString() << '\n';
        return Success;
    }
    if (command == u"pets" && args.size() == 3 && args.at(1) == u"set") {
        PetBehaviourArgs behaviour;
        if (parser.isSet(clickThroughOption))
            behaviour.clickThrough = parser.value(clickThroughOption);
        if (parser.isSet(lockOption))
            behaviour.lock = parser.value(lockOption);
        if (parser.isSet(keepAboveOption))
            behaviour.keepAbove = parser.value(keepAboveOption);
        if (parser.isSet(hoverHideOption))
            behaviour.hoverHide = parser.value(hoverHideOption);
        if (parser.isSet(focusFadeOption))
            behaviour.focusFade = parser.value(focusFadeOption);
        if (parser.isSet(smartDodgeOption))
            behaviour.smartDodge = parser.value(smartDodgeOption);
        if (parser.isSet(rotationOption))
            behaviour.rotation = parser.value(rotationOption);
        if (parser.isSet(flipHOption))
            behaviour.flipH = parser.value(flipHOption);
        if (parser.isSet(flipVOption))
            behaviour.flipV = parser.value(flipVOption);
        QJsonObject changes;
        QString problem;
        if (!petBehaviourChanges(behaviour, &changes, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        if (changes.isEmpty()) {
            err() << "murasakictl: pets set needs at least one option (--click-through, --lock, --keep-above, --hover-hide, --focus-fade, --smart-dodge, --rotation, --flip-h, --flip-v)\n";
            return UsageError;
        }
        if (!requireRevision(kPetBehavioursDBusApiVersion, "the behaviour of a pet"))
            return Failure;
        const QString json = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"UpdatePet"_s, {args.at(2), json}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() == 4 && args.at(1) == u"stage") {
        QJsonObject changes;
        QString problem;
        const std::optional<QString> fit = parser.isSet(fitOption) ? std::optional(parser.value(fitOption)) : std::nullopt;
        const std::optional<QString> over = parser.isSet(overFullscreenOption) ? std::optional(parser.value(overFullscreenOption)) : std::nullopt;
        if (!petStageChanges(args.at(3), fit, over, &changes, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        if (!requireRevision(kPetStageDBusApiVersion, "the fullscreen mode of a pet"))
            return Failure;
        const QString json = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"UpdatePet"_s, {args.at(2), json}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() >= 4 && args.at(1) == u"state") {
        QJsonObject changes;
        QString problem;
        const auto optional = [&](const QCommandLineOption &option) { return parser.isSet(option) ? std::optional(parser.value(option)) : std::nullopt; };
        if (!petStateChanges(args.mid(3), optional(orderOption), optional(advanceOption), parser.isSet(onceOption), optional(holdMsOption), &changes, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        if (!requireRevision(kPetStatesDBusApiVersion, "the states of a pet"))
            return Failure;
        const QString json = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"UpdatePet"_s, {args.at(2), json}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() == 4 && args.at(1) == u"track") {
        QJsonObject changes;
        QString problem;
        const auto optional = [&](const QCommandLineOption &option) { return parser.isSet(option) ? std::optional(parser.value(option)) : std::nullopt; };
        if (!petTrackChanges(args.at(3), optional(distanceOption), optional(speedOption), parser.isSet(returnOption), parser.isSet(noReturnOption), &changes, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        if (!requireRevision(kPetStatesDBusApiVersion, "Mouse Tracking"))
            return Failure;
        const QString json = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"UpdatePet"_s, {args.at(2), json}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() >= 4 && args.at(1) == u"timing") {
        QJsonObject changes;
        QString problem;
        if (!petTimingChanges(args.mid(3), &changes, &problem)) {
            err() << "murasakictl: " << problem << '\n';
            return UsageError;
        }
        if (!requireTimingRevision())
            return Failure;
        const QString json = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));
        return call(kDaemon, u"UpdatePet"_s, {args.at(2), json}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() == 2
        && (args.at(1) == u"hide" || args.at(1) == u"show" || args.at(1) == u"toggle" || args.at(1) == u"toggle-hidden" || args.at(1) == u"rescue")) {
        if (!requireRevision(kPetBehavioursDBusApiVersion, "hiding and rescuing the pets"))
            return Failure;
        if (args.at(1) == u"rescue")
            return call(kDaemon, u"RescuePets"_s) ? Success : Failure;
        bool hidden = args.at(1) == u"hide";
        if (args.at(1) == u"toggle" || args.at(1) == u"toggle-hidden") {
            QDBusMessage get = QDBusMessage::createMethodCall(kDaemon.service, kDaemon.path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
            get.setArguments({kDaemon.interface, u"PetsHidden"_s});
            const QDBusMessage reply = QDBusConnection::sessionBus().call(get);
            if (reply.type() == QDBusMessage::ErrorMessage) {
                err() << "murasakictl: " << reply.errorMessage() << '\n';
                return Failure;
            }
            hidden = !reply.arguments().value(0).value<QDBusVariant>().variant().toBool();
        }
        return call(kDaemon, u"SetPetsHidden"_s, {hidden}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() >= 3 && args.at(1) == u"audio") {
        const QString verb = args.at(2);
        if (!requireRevision(kPetAudioDBusApiVersion, "the sound of a pet"))
            return Failure;
        if (verb == u"import" && args.size() == 4) {
            const auto asset = importSound(args.at(3));
            if (!asset)
                return Failure;
            out() << *asset << '\n';
            return Success;
        }
        if (verb == u"from-item" && args.size() == 4) {
            const auto asset = importSound(args.at(3), true);
            if (!asset)
                return Failure;
            out() << *asset << '\n';
            return Success;
        }
        if (verb == u"list" && args.size() == 3) {
            const auto assets = call(kDaemon, u"ListPetAudioAssets"_s);
            if (!assets)
                return Failure;
            for (const QJsonValue &value : jsonArray(*assets))
                out() << formatPetAudioAssetLine(value.toObject()) << '\n';
            return Success;
        }
        if (verb == u"remove" && args.size() == 4)
            return call(kDaemon, u"RemovePetAudioAsset"_s, {args.at(3)}) ? Success : Failure;
        if (verb == u"gc" && args.size() == 3) {
            const auto removed = call(kDaemon, u"CollectPetAudio"_s);
            if (!removed)
                return Failure;
            out() << removed->toInt() << '\n';
            return Success;
        }
        if (verb == u"off" && args.size() == 4)
            return call(kDaemon, u"UpdatePet"_s, {args.at(3), u"{\"audio\":null}"_s}) ? Success : Failure;
        if (verb == u"test" && args.size() == 4)
            return call(kOverlay, u"PlayPetSound"_s, {args.at(3)}) ? Success : Failure;
        if (verb == u"stop" && args.size() == 3)
            return call(kOverlay, u"StopPetSounds"_s) ? Success : Failure;
        if (verb == u"set" && args.size() == 4) {
            PetAudioArgs audio;
            if (parser.isSet(assetOption))
                audio.asset = parser.value(assetOption);
            if (parser.isSet(modeOption))
                audio.mode = parser.value(modeOption);
            if (parser.isSet(volumeOption))
                audio.volume = parser.value(volumeOption);
            if (parser.isSet(offsetOption))
                audio.offset = parser.value(offsetOption);
            if (parser.isSet(fileOption)) {
                if (audio.asset) {
                    err() << "murasakictl: use --asset or --file, not both\n";
                    return UsageError;
                }
                const auto asset = importSound(parser.value(fileOption));
                if (!asset)
                    return Failure;
                audio.asset = *asset;
            }
            QJsonObject changes;
            QString problem;
            if (!petAudioChanges(audio, &changes, &problem)) {
                err() << "murasakictl: " << problem << '\n';
                return UsageError;
            }
            if (changes.isEmpty()) {
                err() << "murasakictl: pets audio set needs at least one option (--asset or --file, --mode, --volume, --offset)\n";
                return UsageError;
            }
            return call(kDaemon, u"UpdatePet"_s, {args.at(3), QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact))}) ? Success : Failure;
        }
        err() << "murasakictl: pets audio import <file> | from-item <item> | list | remove <id> | gc | set <pet> ... | off <pet> | test <pet> | stop\n";
        return UsageError;
    }
    if (command == u"pets" && args.size() >= 2 && (args.at(1) == u"volume" || args.at(1) == u"mute" || args.at(1) == u"unmute")) {
        if (!requireRevision(kPetAudioDBusApiVersion, "the volume of the pets"))
            return Failure;
        QJsonObject settings;
        if (args.at(1) == u"volume" && args.size() == 3) {
            bool ok = false;
            const int volume = args.at(2).toInt(&ok);
            if (!ok || volume < 0 || volume > 100) {
                err() << "murasakictl: the volume is a whole number from 0 to 100\n";
                return UsageError;
            }
            settings.insert(u"petsVolume"_s, volume);
        } else if (args.at(1) != u"volume" && args.size() == 2) {
            settings.insert(u"petsMuted"_s, args.at(1) == u"mute");
        } else {
            err() << "murasakictl: pets volume <0-100> | mute | unmute\n";
            return UsageError;
        }
        return call(kDaemon, u"SetSettings"_s, {QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact))}) ? Success : Failure;
    }
    if (command == u"pets" && args.size() == 3 && args.at(1) == u"remove")
        return call(kDaemon, u"RemovePet"_s, {args.at(2)}) ? Success : Failure;
    if (command == u"pets" && args.size() == 3 && args.at(1) == u"arrange" && arrangeTarget(args.at(2), false))
        return setSessionFlag(u"PetsArrange"_s, u"SetPetsArrange"_s, args.at(2)) ? Success : Failure;
    if (command == u"wallpaper" && args.size() == 3 && args.at(1) == u"interact" && arrangeTarget(args.at(2), false)) {
        if (!requireRevision(kWallpaperInteractDBusApiVersion, "interacting with the wallpaper"))
            return Failure;
        return setSessionFlag(u"WallpaperInteract"_s, u"SetWallpaperInteract"_s, args.at(2)) ? Success : Failure;
    }
    if (command == u"steamcmd" && args.size() >= 2)
        return steamCmdCommand(parser, args, appOption);
    if (command == u"profile" && args.size() >= 2)
        return profileCommand(args);
    if (command == u"triggers" && args.size() >= 3 && args.at(1) == u"screen")
        return screenCommand(parser, args, stepsOption, saveToOption, yesOption);
    if (command == u"triggers" && args.size() >= 2)
        return triggersCommand(args);
    if (command == u"timeline" && args.size() >= 2)
        return timelineCommand(args);
    if (command == u"demo")
        return demoCommand(parser, args, screenOption, petOption);
    if (command == u"pets" && args.size() >= 3 && args.at(1) == u"runtime" && args.at(2) == u"clear") {
        if (!requireRevision(kAutomationDBusApiVersion, "the runtime layer of the pets"))
            return Failure;
        return call(kDaemon, u"ClearPetRuntime"_s, {args.value(3)}) ? Success : Failure;
    }
    if (command == u"playlist" && args.size() >= 2)
        return playlistCommand(parser, args, output, desktopOption, outputOption, fillOption, muteOption, spanOption);
    err() << parser.helpText();
    return UsageError;
}
