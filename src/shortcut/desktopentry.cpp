#include "shortcut/desktopentry.h"

#include "appclass.h"
#include "shortcut/shortcutpolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QStringDecoder>

#include <algorithm>
#include <set>

namespace murasaki::shortcut {

namespace {

// A value with the string escapes of the specification (\s \n \t \r \\) applied; other backslashes stay.
QString unescape(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (raw[i] == QLatin1Char('\\') && i + 1 < raw.size()) {
            const QChar next = raw[i + 1];
            if (next == QLatin1Char('s')) { out += QLatin1Char(' '); ++i; continue; }
            if (next == QLatin1Char('n')) { out += QLatin1Char('\n'); ++i; continue; }
            if (next == QLatin1Char('t')) { out += QLatin1Char('\t'); ++i; continue; }
            if (next == QLatin1Char('r')) { out += QLatin1Char('\r'); ++i; continue; }
            if (next == QLatin1Char('\\')) { out += QLatin1Char('\\'); ++i; continue; }
        }
        out += raw[i];
    }
    return out;
}

bool truthy(const QString &value)
{
    return value == QLatin1String("true") || value == QLatin1String("1");
}

QString cutBytes(const QString &text, int limit)
{
    QString out = text;
    while (out.toUtf8().size() > limit && !out.isEmpty()) {
        out.chop(1);
        if (!out.isEmpty() && out.back().isHighSurrogate()) {
            out.chop(1);
        }
    }
    return out;
}

QStringList splitList(const QString &value)
{
    QStringList out;
    for (const QString &part : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        out << part.trimmed();
    }
    return out;
}

// The best value among Name, Name[lang_COUNTRY], Name[lang]...: the first locale in the list that has one, else the plain key.
QString localised(const QHash<QString, QString> &values, const QString &key, const QStringList &locales)
{
    for (const QString &locale : locales) {
        if (const auto it = values.constFind(key + QLatin1Char('[') + locale + QLatin1Char(']')); it != values.constEnd() && !it->isEmpty()) {
            return *it;
        }
    }
    return values.value(key);
}

} // namespace

DesktopEnvironment systemDesktopEnvironment()
{
    DesktopEnvironment env;
    env.currentDesktops = QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    const QString name = QLocale::system().name(); // "es_MX"
    if (!name.isEmpty() && name != QLatin1String("C")) {
        env.locales << name;
        if (const qsizetype underscore = name.indexOf(QLatin1Char('_')); underscore > 0) {
            env.locales << name.left(underscore);
        }
    }
    env.hasExecutable = [](const QString &program) {
        if (program.isEmpty()) {
            return false;
        }
        if (QDir::isAbsolutePath(program)) {
            const QFileInfo info(program);
            return info.isFile() && info.isExecutable();
        }
        return !QStandardPaths::findExecutable(program).isEmpty();
    };
    return env;
}

namespace {

// What the windows of an Exec line are called: the program's basename, `flatpak run ... app.id` as the app id, and
// `steam://rungameid/N` as steam_app_N.
QStringList execClassNames(const QString &exec)
{
    QStringList names;
    bool flatpak = false;
    bool program = false;
    for (QString word : exec.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (word.size() >= 2 && word.startsWith(QLatin1Char('"')) && word.endsWith(QLatin1Char('"'))) {
            word = word.mid(1, word.size() - 2);
        }
        static const QString steamGame = QStringLiteral("steam://rungameid/");
        if (word.startsWith(steamGame)) {
            const QString number = word.mid(steamGame.size());
            if (!number.isEmpty() && std::all_of(number.begin(), number.end(), [](QChar c) { return c.isDigit(); })) {
                return {QStringLiteral("steam_app_") + number}; // (the game's windows, not Steam's)
            }
            continue;
        }
        if (word == QLatin1String("env") || word.contains(QLatin1Char('=')) || word.startsWith(QLatin1Char('%')) || word.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (flatpak) {
            if (word != QLatin1String("run")) {
                names << word;
                break;
            }
            continue;
        }
        if (!program) {
            program = true;
            const QString base = word.mid(word.lastIndexOf(QLatin1Char('/')) + 1);
            if (base == QLatin1String("flatpak")) {
                flatpak = true;
            } else {
                names << base;
            }
        }
    }
    return names;
}

QStringList classAliasesFor(const DesktopEntry &entry, const QString &id)
{
    QStringList candidates;
    candidates << (id.endsWith(QLatin1String(".desktop")) ? id.chopped(8) : id);
    const QString last = murasaki::lastAppClassComponent(murasaki::normalizedAppClass(candidates.constFirst()));
    if (!last.isEmpty()) {
        candidates << last;
    }
    candidates << execClassNames(entry.exec);
    QStringList aliases;
    for (const QString &name : std::as_const(candidates)) {
        const QString clean = cutBytes(name, kMaxFieldBytes);
        const bool controls = std::any_of(clean.begin(), clean.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
        const bool known = std::any_of(aliases.begin(), aliases.end(), [&](const QString &a) { return a.compare(clean, Qt::CaseInsensitive) == 0; });
        if (!clean.isEmpty() && !controls && !known && clean.compare(entry.windowClass, Qt::CaseInsensitive) != 0 && aliases.size() < 8) {
            aliases << clean;
        }
    }
    return aliases;
}

} // namespace

std::optional<DesktopEntry> parseDesktopEntry(const QByteArray &data, const QString &id, const DesktopEnvironment &env)
{
    if (data.isEmpty() || data.size() > kMaxDesktopFileBytes || data.contains('\0')) {
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder(data);
    if (decoder.hasError()) {
        return std::nullopt;
    }
    QHash<QString, QString> values;
    bool inEntry = false, seenEntry = false;
    for (const QString &rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            seenEntry = seenEntry || inEntry;
            if (!inEntry && seenEntry) {
                break; // an action group or anything after the main one: not read
            }
            continue;
        }
        if (!inEntry) {
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QString key = line.left(eq).trimmed();
        if (key.size() > 64 || values.contains(key)) {
            continue; // the first one wins
        }
        values.insert(key, line.mid(eq + 1).trimmed());
    }
    if (!seenEntry || values.value(QStringLiteral("Type")) != QLatin1String("Application")) {
        return std::nullopt;
    }
    if (truthy(values.value(QStringLiteral("Hidden"))) || truthy(values.value(QStringLiteral("NoDisplay")))) {
        return std::nullopt;
    }
    const QStringList only = splitList(values.value(QStringLiteral("OnlyShowIn")));
    const QStringList not_ = splitList(values.value(QStringLiteral("NotShowIn")));
    const auto anyOf = [&](const QStringList &list) {
        return std::any_of(env.currentDesktops.begin(), env.currentDesktops.end(), [&](const QString &d) { return list.contains(d); });
    };
    if (!only.isEmpty() && !anyOf(only)) {
        return std::nullopt;
    }
    if (!not_.isEmpty() && anyOf(not_)) {
        return std::nullopt;
    }
    if (const QString tryExec = unescape(values.value(QStringLiteral("TryExec"))); !tryExec.isEmpty()) {
        if (cutBytes(tryExec, kMaxFieldBytes) != tryExec || !env.hasExecutable || !env.hasExecutable(tryExec)) {
            return std::nullopt;
        }
    }
    DesktopEntry entry;
    entry.id = id;
    entry.name = cleanDisplayName(unescape(localised(values, QStringLiteral("Name"), env.locales)));
    entry.icon = cutBytes(unescape(values.value(QStringLiteral("Icon"))), kMaxFieldBytes);
    entry.exec = unescape(values.value(QStringLiteral("Exec")));
    entry.terminal = truthy(values.value(QStringLiteral("Terminal")));
    if (entry.name.isEmpty() || entry.exec.isEmpty() || entry.exec.toUtf8().size() > kMaxExecBytes) {
        return std::nullopt;
    }
    const auto hasControls = [](const QString &text) {
        return std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
    };
    // an icon name or an absolute path with control characters is no icon
    if (hasControls(entry.icon)) {
        entry.icon.clear();
    }
    entry.windowClass = cutBytes(unescape(values.value(QStringLiteral("StartupWMClass"))), kMaxFieldBytes);
    if (entry.windowClass.isEmpty() || hasControls(entry.windowClass)) {
        // A Steam shortcut's game names its windows steam_app_N, not after the file.
        const QStringList exec = execClassNames(entry.exec);
        const bool steamGame = exec.size() == 1 && exec.constFirst().startsWith(QLatin1String("steam_app_"));
        entry.windowClass = steamGame ? exec.constFirst() : id.endsWith(QLatin1String(".desktop")) ? id.chopped(8) : id;
    }
    entry.classAliases = classAliasesFor(entry, id);
    return entry;
}

QStringList applicationRoots()
{
    QStringList roots;
    const auto add = [&](const QString &dir) {
        const QString clean = QDir::cleanPath(dir);
        if (!clean.isEmpty() && QDir::isAbsolutePath(clean) && !roots.contains(clean)) {
            roots << clean;
        }
    };
    const QString dataHome = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    add((dataHome.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : dataHome) + QStringLiteral("/applications"));
    QString dirs = QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS"));
    if (dirs.isEmpty()) {
        dirs = QStringLiteral("/usr/local/share:/usr/share");
    }
    for (const QString &d : dirs.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        add(d + QStringLiteral("/applications"));
    }
    add(QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications"));
    add(QStringLiteral("/var/lib/flatpak/exports/share/applications"));
    add(QStringLiteral("/var/lib/snapd/desktop/applications"));
    return roots;
}

namespace {

std::optional<DesktopEntry> readFile(const QString &path, const QString &id, const DesktopEnvironment &env)
{
    const QFileInfo info(path);
    const QString real = info.canonicalFilePath(); // symlinks are followed (Flatpak exports are links); only a regular file counts
    if (real.isEmpty()) {
        return std::nullopt;
    }
    const QFileInfo target(real);
    if (!target.isFile() || target.size() > kMaxDesktopFileBytes) {
        return std::nullopt;
    }
    QFile file(real);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray data = file.read(kMaxDesktopFileBytes + 1);
    auto entry = parseDesktopEntry(data, id, env);
    if (entry) {
        entry->path = real;
    }
    return entry;
}

} // namespace

std::vector<DesktopEntry> scanApplications(const QStringList &roots, const DesktopEnvironment &env)
{
    std::vector<DesktopEntry> out;
    std::set<QString> seen; // ids found, so a later root does not replace an earlier one (even when that one was hidden)
    int looked = 0;
    for (const QString &root : roots) {
        const QDir dir(root);
        if (!dir.exists()) {
            continue;
        }
        const auto visit = [&](const QDir &d, const QString &prefix) {
            const QStringList names = d.entryList({QStringLiteral("*.desktop")}, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &name : names) {
                if (++looked > kMaxDesktopFiles || static_cast<int>(out.size()) >= kMaxDesktopEntries) {
                    return false;
                }
                const QString id = prefix + name;
                if (!seen.insert(id).second) {
                    continue;
                }
                if (auto entry = readFile(d.filePath(name), id, env)) {
                    out.push_back(std::move(*entry));
                }
            }
            return true;
        };
        if (!visit(dir, QString())) {
            break;
        }
        bool more = true;
        for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (!more) {
                break;
            }
            more = visit(QDir(dir.filePath(sub)), sub + QLatin1Char('-'));
        }
        if (!more) {
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const DesktopEntry &a, const DesktopEntry &b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return out;
}

std::optional<DesktopEntry> findApplication(const QStringList &roots, const QString &id, const DesktopEnvironment &env)
{
    // a valid id is a file name: no slash, no dot-dot, ends in .desktop
    if (id.isEmpty() || id.size() > kMaxFieldBytes || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\0')) || !id.endsWith(QLatin1String(".desktop"))
        || id.startsWith(QLatin1Char('.')) || id.startsWith(QLatin1Char('-'))) { // (an id is never read as an option by gtk-launch)
        return std::nullopt;
    }
    for (const QString &root : roots) {
        const QString plain = QDir(root).filePath(id);
        if (QFileInfo::exists(plain)) {
            return readFile(plain, id, env); // the first root that has the id decides, hidden or not
        }
        // "a-b.desktop" may also be "a/b.desktop" (one level)
        for (qsizetype dash = id.indexOf(QLatin1Char('-')); dash > 0; dash = id.indexOf(QLatin1Char('-'), dash + 1)) {
            const QString nested = QDir(root).filePath(id.left(dash) + QLatin1Char('/') + id.mid(dash + 1));
            if (QFileInfo::exists(nested)) {
                return readFile(nested, id, env);
            }
        }
    }
    return std::nullopt;
}

} // namespace murasaki::shortcut
