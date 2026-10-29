#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace murasaki {

inline constexpr qint64 kMaxJsonFileSize = 4 * 1024 * 1024;

// Shared by the daemon's stores (assignments.json, settings.json).
struct JsonFileRead {
    bool exists = false;
    QJsonObject root; // valid only when exists and problem is empty
    QString problem;  // why an existing file cannot be used
};

// Reads a regular JSON file of at most 4 MiB whose top level is an object.
JsonFileRead readJsonObjectFile(const QString &path);

// Renames an unusable file to <path>.bad-<UTC time> and logs it; fallback names what the daemon uses instead
// (for example "no assignments") and noun what is lost if it cannot be renamed ("assignments"). Returns false when
// the rename failed, in which case callers must not overwrite the file.
bool quarantineBadFile(const QString &path, const QString &problem, const QString &fallback, const QString &noun);

// Atomic write (QSaveFile), creating the parent dir. Logs a warning and returns false on failure.
bool writeJsonObjectFile(const QString &path, const QJsonObject &root);

} // namespace murasaki
