#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include <optional>
#include <utility>

namespace murasaki {

// Matches any output or any desktop in an assignment key.
inline constexpr QLatin1String kWildcard("*");

// One wallpaper choice for an (output, desktop) key. output is a connector name (DP-1) and desktop a compositor
// desktop id; either may be kWildcard.
struct Assignment {
    QString output;
    QString desktop;
    QString itemId;
    bool muted = false;
    QString fill = QStringLiteral("fill"); // "fill" (cover and crop) | "fit" | "stretch"
    // One wallpaper across all outputs as a single canvas (see canvas.h); only valid when output is kWildcard.
    // With it on, fill applies to the whole canvas.
    bool span = false;
    QJsonObject properties;                // user property overrides, written by the GUI

    QJsonObject toJson() const;
    bool operator==(const Assignment &other) const = default;
};

// 1-200 characters: the D-Bus API echoes keys in errors and stores them on disk.
bool isValidKeyPart(const QString &part);

// Validates D-Bus or file input. options may hold muted (bool), fill (string), span (bool, only with output "*") and properties (object); other keys
// are ignored so the file and the API can grow.
std::optional<Assignment> makeAssignment(const QString &output, const QString &desktop, const QString &itemId,
                                         const QJsonObject &options, QString *error = nullptr);

// The daemon's persistent assignments ($XDG_CONFIG_HOME/murasaki/assignments.json).
class AssignmentStore : public QObject
{
    Q_OBJECT

public:
    explicit AssignmentStore(QString filePath, QObject *parent = nullptr);

    QString filePath() const { return m_path; }

    // A missing file means no assignments. An unreadable or invalid file is renamed to <file>.bad-<UTC time> and
    // the store starts empty; if that rename fails, saving stays disabled so the user's file is never overwritten.
    void load();

    QList<Assignment> assignments() const; // sorted by (output, desktop)

    // First existing key of (output, desktop), (output, *), (*, desktop), (*, *).
    std::optional<Assignment> resolve(const QString &output, const QString &desktop) const;

    bool referencesItem(const QString &itemId) const;

    // Both return false without saving or signalling when nothing changes.
    bool set(const Assignment &assignment);
    bool clear(const QString &output, const QString &desktop);

signals:
    void changed();

private:
    using Key = std::pair<QString, QString>;

    void save() const;

    QString m_path;
    QMap<Key, Assignment> m_entries;
    bool m_saveBlocked = false;
};

} // namespace murasaki
