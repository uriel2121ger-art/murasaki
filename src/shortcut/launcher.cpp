#include "shortcut/launcher.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <algorithm>

namespace murasaki::shortcut {

FindProgram systemFindProgram()
{
    return [](const QString &name) {
        if (QDir::isAbsolutePath(name)) {
            const QFileInfo info(name);
            return info.isFile() && info.isExecutable() ? name : QString();
        }
        return QStandardPaths::findExecutable(name);
    };
}

std::optional<QStringList> parseExec(const QString &exec)
{
    if (exec.isEmpty() || exec.toUtf8().size() > kMaxExecBytes) {
        return std::nullopt;
    }
    // characters that may only appear inside double quotes (the specification's reserved set)
    static const QString reserved = QStringLiteral("\t\n\"'\\><~|&;$*?#()`");
    static const QString fieldCodes = QStringLiteral("fFuUicdDnNvmk");
    QStringList args;
    QString current;
    bool started = false;  // an argument is being read (its first character, or its opening quote, was seen)
    bool literal = false;  // it holds text of its own (not only field codes)
    bool hadQuotes = false, hadCode = false;
    bool quoted = false;
    const auto finish = [&] {
        // an argument made of field codes alone is not one (they are dropped and so is it, quoted or not); an explicit "" stays
        if (started && (literal || !current.isEmpty() || (hadQuotes && !hadCode))) {
            args << current;
        }
        current.clear();
        started = literal = hadQuotes = hadCode = false;
    };
    // a `%` and the character after it: `%%` is a percent sign, a field code is dropped, anything else refuses the line
    const auto percent = [&](qsizetype &i) -> bool {
        if (i + 1 >= exec.size()) {
            return false;
        }
        const QChar code = exec[++i];
        if (code == QLatin1Char('%')) {
            current += QLatin1Char('%');
            literal = true;
        } else if (!fieldCodes.contains(code)) {
            return false;
        } else {
            hadCode = true;
        }
        return true;
    };
    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec[i];
        if (quoted) {
            if (c == QLatin1Char('"')) {
                quoted = false;
            } else if (c == QLatin1Char('\\')) {
                // inside quotes a backslash escapes one of " ` $ \ and nothing else
                if (i + 1 >= exec.size() || !QStringLiteral("\"`$\\").contains(exec[i + 1])) {
                    return std::nullopt;
                }
                current += exec[++i];
                literal = true;
            } else if (c == QLatin1Char('%')) {
                if (!percent(i)) {
                    return std::nullopt;
                }
            } else {
                current += c;
                literal = true;
            }
            continue;
        }
        if (c == QLatin1Char(' ')) {
            finish();
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted = true;
            started = true;
            hadQuotes = true;
            continue;
        }
        if (c == QLatin1Char('%')) {
            started = true;
            if (!percent(i)) {
                return std::nullopt;
            }
            continue;
        }
        if (reserved.contains(c) || c.unicode() < 0x20) {
            return std::nullopt;
        }
        current += c;
        started = true;
        literal = true;
    }
    if (quoted) {
        return std::nullopt;
    }
    finish();
    if (args.isEmpty() || args.size() > 64 || args.first().isEmpty()) {
        return std::nullopt;
    }
    return args;
}

std::optional<LaunchCommand> planLaunch(const DesktopEntry &entry, const FindProgram &find, QString *why)
{
    const auto fail = [&](const QString &reason) -> std::optional<LaunchCommand> {
        if (why) {
            *why = reason;
        }
        return std::nullopt;
    };
    if (entry.path.isEmpty() || !QDir::isAbsolutePath(entry.path) || entry.id.isEmpty() || entry.id.startsWith(QLatin1Char('-'))) {
        return fail(QStringLiteral("the entry has no file"));
    }
    if (const QString gio = find(QStringLiteral("gio")); !gio.isEmpty()) {
        return LaunchCommand{gio, {QStringLiteral("launch"), entry.path}, QStringLiteral("gio")};
    }
    if (const QString gtk = find(QStringLiteral("gtk-launch")); !gtk.isEmpty()) {
        return LaunchCommand{gtk, {QStringLiteral("--"), entry.id}, QStringLiteral("gtk-launch")}; // (an id never starts with '-': findApplication)
    }
    // last resort: the Exec line itself, never for an entry that wants a terminal
    if (entry.terminal) {
        return fail(QStringLiteral("it needs a terminal and neither gio nor gtk-launch is installed"));
    }
    const std::optional<QStringList> argv = parseExec(entry.exec);
    if (!argv) {
        return fail(QStringLiteral("its Exec line is not one that can be read safely"));
    }
    const QString program = find(argv->first());
    if (program.isEmpty() || !QDir::isAbsolutePath(program)) {
        return fail(QStringLiteral("its program was not found"));
    }
    return LaunchCommand{program, argv->mid(1), QStringLiteral("exec")};
}

bool startDetached(const LaunchCommand &command)
{
    QProcess process;
    process.setProgram(command.program);
    process.setArguments(command.arguments);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &key : {QStringLiteral("GIO_LAUNCHED_DESKTOP_FILE"), QStringLiteral("GIO_LAUNCHED_DESKTOP_FILE_PID"), QStringLiteral("DESKTOP_STARTUP_ID"),
                               QStringLiteral("XDG_ACTIVATION_TOKEN")}) {
        env.remove(key);
    }
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    return process.startDetached();
}

bool launch(const DesktopEntry &entry, const FindProgram &find, const std::function<bool(const LaunchCommand &)> &start, QString *why)
{
    const std::optional<LaunchCommand> command = planLaunch(entry, find, why);
    if (!command) {
        return false;
    }
    if (!start(*command)) {
        if (why) {
            *why = QStringLiteral("the program did not start");
        }
        return false;
    }
    return true;
}

} // namespace murasaki::shortcut
