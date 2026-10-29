#pragma once

// Window classes as compositors report them (WM_CLASS, Wayland app_id) against the names a user or a .desktop file gives an
// application. The same program shows up as "org.kde.dolphin", "dolphin" or "Dolphin" depending on the toolkit and the desktop file,
// so a rule without wildcards also matches these spellings. Header only: the trigger engine and the pause rules both use it.

#include <QString>
#include <QStringList>

namespace murasaki {

// The class folded for comparison: trimmed, case-folded, without a ".desktop" suffix (some compositors report the file id).
inline QString normalizedAppClass(const QString &appClass)
{
    QString folded = appClass.trimmed().toCaseFolded();
    if (folded.endsWith(u".desktop") && folded.size() > 8)
        folded.chop(8);
    return folded;
}

// The last component of a reverse-DNS class ("org.kde.dolphin" -> "dolphin"), or empty when the class has fewer than three parts.
inline QString lastAppClassComponent(const QString &normalized)
{
    const QStringList parts = normalized.split(u'.');
    return parts.size() >= 3 && !parts.last().isEmpty() ? parts.last() : QString();
}

// Whether two classes name the same application without wildcards: equal after normalising, or a bare name against the last
// component of a reverse-DNS id. Two different reverse-DNS ids never match each other by their tails.
inline bool sameAppClass(const QString &a, const QString &b)
{
    const QString x = normalizedAppClass(a), y = normalizedAppClass(b);
    if (x.isEmpty() || y.isEmpty())
        return false;
    if (x == y)
        return true;
    if (!y.contains(u'.') && lastAppClassComponent(x) == y)
        return true;
    return !x.contains(u'.') && lastAppClassComponent(y) == x;
}

} // namespace murasaki
