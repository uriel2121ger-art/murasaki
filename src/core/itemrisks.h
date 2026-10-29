#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace murasaki {

// How deep (directories below the item dir), how many files and how many findings one scan looks at. Workshop
// items are small; a bigger tree is cut off rather than walked.
inline constexpr int kRiskScanMaxDepth = 6;
inline constexpr int kRiskScanMaxFiles = 2000;
inline constexpr int kRiskListMax = 20;

// A file of an item that is (or may be) a program. Murasaki never runs these; they are reported so the user knows
// the item is not just media.
struct ItemRisk {
    QString kind; // "executable" (by content or name) or "application" (project.json type)
    QString file; // path relative to the item dir

    QJsonObject toJson() const;
};

// Scans an item folder for programs: ELF, PE and Mach-O magics, shebang scripts, and the extensions .exe .dll .scr
// .bat .cmd .ps1 .vbs .sh .AppImage .jar .msi .lnk. JavaScript and other web files are never flagged by name or
// shebang, since web wallpapers are made of them. Symlinks are not followed.
QList<ItemRisk> scanItemRisks(const QString &itemDir);

// The same check for one file (a local pet animation); the finding's file is the file name.
QList<ItemRisk> scanFileRisk(const QString &filePath);

} // namespace murasaki
