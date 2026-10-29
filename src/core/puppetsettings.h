#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace murasaki {

// The settings of the puppet pets (docs/pets/puppets.md section 22), in the daemon's settings. The user's rule: whatever depends on the user is a setting with a safe default.
//
//   puppetMaxPixels  int, kMinPuppetMaxPixels to kMaxPuppetMaxPixels (default kDefaultPuppetMaxPixels = 2048 x 2048): the most pixels the picture of one puppet pet is drawn at; a bigger pet is drawn smaller
//                    and scaled up by the window (it says `puppet: resolution reduced`). The engine's own cap (the long side, kMaxPuppetPixels) holds anyway.
//   puppetCorePath   string, empty by default: the Cubism Core library the user installed (Murasaki ships none). It is **only stored and checked here**: nothing in Murasaki loads or opens it yet (the Cubism back end,
//                    task 9, waits for the licence question of the plan). A path is accepted when it is empty or absolute, without control characters, at most 4096 bytes; whether the file is there and
//                    is safe to load (a regular file, owned by the user or root, writable by nobody else) is `coreFileProblem()`, which `SetSettings` asks for before it stores a path.
struct PuppetSettings {
    static constexpr long long kMinMaxPixels = 1000000;
    static constexpr long long kMaxMaxPixels = 16000000;
    static constexpr long long kDefaultMaxPixels = 4194304;

    long long maxPixels = kDefaultMaxPixels;
    QString corePath;

    bool operator==(const PuppetSettings &) const = default;

    enum class Result { NotMine, Ok, Error };
    Result apply(const QString &key, const QJsonValue &value, QString *error);
    void write(QJsonObject &into) const;
    static QStringList keys();

    // Why the file at `path` must not be loaded as a Core library (empty when it may be): not a regular file, owned by someone else than the user or root, or writable by group or others. Looks at the file
    // system, so it is not part of apply(): a settings file whose library has since gone must still load.
    static QString coreFileProblem(const QString &path);
};

} // namespace murasaki
