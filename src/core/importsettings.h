#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace murasaki {

// The defaults of the pet imports (docs/pets/import.md), kept in the daemon's settings so the GUI and murasakictl start a form from them and the daemon
// applies the ones that are its own (convertGifOnImport, the Workshop policies, the output budget). The user's rule: whatever depends on the user is a
// setting with a safe default. Each import can override its own options in the job.
struct ImportSettings {
    QString format = QStringLiteral("apng");        // importFormat: apng | webp
    QString sizePolicy = QStringLiteral("fit");     // importSizePolicy: fit (letterbox) | scale | first (refuse)
    int fps = 24;                                   // importFps: 1..120, for sources without timing
    bool keepSource = false;                        // importKeepSource
    bool keepAudio = false;                         // importKeepAudio: a video's audio track, kept for phase 5
    bool convertGifOnImport = false;                // convertGifOnImport: also convert a GIF to APNG when it is added as a pet
    QString workshopFiles = QStringLiteral("ask");  // importWorkshopFiles: ask | auto, for a ZIP or video inside a Workshop item
    QString workshopMdaa = QStringLiteral("ask");   // importWorkshopMdaa: ask | auto, for an .mdaa package inside a Workshop item
    int maxOutputMiB = 1024;                        // importMaxOutputMiB: 16..1024, the most an import may write
    int videoMaxSide = 1024;                        // importVideoMaxSide: 64..8192
    int videoMaxFps = 30;                           // importVideoMaxFps: 1..60
    bool requireSandbox = true;                     // importRequireSandbox: an import runs only confined (Landlock); off lets it run without, and says so on the job

    bool operator==(const ImportSettings &) const = default;

    enum class Result { NotMine, Ok, Error };
    // Applies one key if it is one of these; Error sets *error.
    Result apply(const QString &key, const QJsonValue &value, QString *error);
    void write(QJsonObject &into) const;
    static QStringList keys();
};

} // namespace murasaki
