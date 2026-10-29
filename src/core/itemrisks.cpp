#include "itemrisks.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QtEndian>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

bool hasRiskyExtension(const QString &name)
{
    static const QSet<QString> extensions{u"exe"_s, u"dll"_s, u"scr"_s, u"bat"_s, u"cmd"_s, u"ps1"_s,
                                          u"vbs"_s, u"sh"_s,  u"appimage"_s, u"jar"_s, u"msi"_s, u"lnk"_s};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}

bool isWebFile(const QString &name)
{
    static const QSet<QString> extensions{u"js"_s, u"mjs"_s, u"cjs"_s, u"html"_s, u"htm"_s, u"css"_s, u"json"_s};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}

quint32 readLe32(const QByteArray &data, int offset)
{
    quint32 value = 0;
    for (int i = 3; i >= 0; --i)
        value = (value << 8) | quint8(data.at(offset + i));
    return value;
}

// `base` and `size` delimit the content inside the file (a package entry); a plain file is base 0, its whole size.
bool hasExecutableContent(QFile &file, qint64 base, qint64 size, bool allowShebang)
{
    if (!file.seek(base))
        return false;
    const QByteArray head = file.read(qMin<qint64>(0x40, size));
    if (head.size() < 2)
        return false;
    if (allowShebang && head.startsWith("#!"))
        return true;
    if (head.size() >= 4) {
        if (head.startsWith("\x7f" "ELF"))
            return true;
        static const char *const machO[] = {"\xfe\xed\xfa\xce", "\xfe\xed\xfa\xcf", "\xce\xfa\xed\xfe", "\xcf\xfa\xed\xfe",
                                           "\xca\xfe\xba\xbe"};
        for (const char *magic : machO) {
            if (head.startsWith(QByteArray(magic, 4)))
                return true;
        }
    }
    // PE: "MZ", and e_lfanew (offset 0x3c) points at "PE\0\0". A bare "MZ" is not enough: it opens plenty of text.
    if (head.size() >= 0x40 && head.startsWith("MZ")) {
        const quint32 peOffset = readLe32(head, 0x3c);
        if (peOffset <= quint64(size) && size - peOffset >= 4 && file.seek(base + peOffset))
            return file.read(4) == QByteArray("PE\0\0", 4);
    }
    return false;
}

bool fileIsRisky(const QString &path, const QString &name)
{
    if (hasRiskyExtension(name))
        return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return hasExecutableContent(file, 0, file.size(), !isWebFile(name));
}

constexpr quint32 kPkgMaxEntries = 20000;
constexpr quint32 kPkgMaxNameLength = 4096;

// Reads a Wallpaper Engine scene package (version string, entry count, then name/offset/length per entry; the data
// follows the table, offsets relative to it) and returns the names of the entries that are programs. Bounded: a
// malformed or huge table ends the scan quietly.
QStringList scanPackage(const QString &path, int room)
{
    QStringList found;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return found;
    const qint64 fileSize = file.size();
    const auto u32 = [&file](quint32 &out) {
        const QByteArray raw = file.read(4);
        if (raw.size() != 4)
            return false;
        out = qFromLittleEndian<quint32>(raw.constData());
        return true;
    };
    quint32 versionLength = 0;
    if (!u32(versionLength) || versionLength > 64 || !file.read(versionLength).startsWith("PKGV"))
        return found;
    quint32 count = 0;
    if (!u32(count) || count > kPkgMaxEntries)
        return found;
    struct Entry { QString name; quint32 offset; quint32 length; };
    QList<Entry> entries;
    for (quint32 i = 0; i < count; ++i) {
        quint32 nameLength = 0, offset = 0, length = 0;
        if (!u32(nameLength) || nameLength > kPkgMaxNameLength)
            return found;
        const QByteArray name = file.read(nameLength);
        if (name.size() != qsizetype(nameLength) || !u32(offset) || !u32(length))
            return found;
        entries.append({QString::fromUtf8(name).replace(u'\\', u'/'), offset, length});
    }
    const qint64 dataStart = file.pos();
    for (const Entry &entry : std::as_const(entries)) {
        if (found.size() >= room)
            break;
        if (dataStart + qint64(entry.offset) + entry.length > fileSize)
            continue;
        bool risky = hasRiskyExtension(entry.name);
        if (!risky && entry.length >= 2)
            risky = hasExecutableContent(file, dataStart + entry.offset, entry.length, !isWebFile(entry.name));
        if (risky)
            found.append(entry.name);
    }
    return found;
}

struct Walk {
    QString root;
    QList<ItemRisk> risks;
    int visited = 0;

    bool done() const { return visited >= kRiskScanMaxFiles || risks.size() >= kRiskListMax; }

    // Directories and files both count against the budget, and a directory is read entry by entry: a hostile folder with
    // millions of entries costs a bounded number of them, not a list of all.
    void scan(const QString &dir, int depth)
    {
        QStringList subdirs;
        QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            if (done())
                return;
            it.next();
            ++visited;
            const QFileInfo file = it.fileInfo();
            if (file.isDir()) {
                subdirs.append(file.absoluteFilePath());
                continue;
            }
            const QString relative = QDir(root).relativeFilePath(file.absoluteFilePath());
            if (fileIsRisky(file.absoluteFilePath(), file.fileName()))
                risks.append({u"executable"_s, relative});
            else if (file.suffix().compare(u"pkg"_s, Qt::CaseInsensitive) == 0) {
                for (const QString &inside : scanPackage(file.absoluteFilePath(), kRiskListMax - int(risks.size())))
                    risks.append({u"executable"_s, relative + u':' + inside});
            }
        }
        if (depth >= kRiskScanMaxDepth)
            return;
        subdirs.sort();
        for (const QString &sub : std::as_const(subdirs)) {
            if (done())
                return;
            scan(sub, depth + 1);
        }
    }
};

} // namespace

QJsonObject ItemRisk::toJson() const
{
    return QJsonObject{{u"kind"_s, kind}, {u"file"_s, file}};
}

QList<ItemRisk> scanItemRisks(const QString &itemDir)
{
    Walk walk{QFileInfo(itemDir).absoluteFilePath(), {}, 0};
    walk.scan(walk.root, 0);
    return walk.risks;
}

QList<ItemRisk> scanFileRisk(const QString &filePath)
{
    const QFileInfo info(filePath);
    if (info.isSymLink() || !info.isFile())
        return {};
    if (fileIsRisky(info.absoluteFilePath(), info.fileName()))
        return {{u"executable"_s, info.fileName()}};
    return {};
}

} // namespace murasaki
