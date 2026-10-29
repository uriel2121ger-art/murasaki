#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include "constants.h"
#include "automationsettings.h"
#include "puppetsettings.h"
#include "importsettings.h"
#include "petstage.h"

namespace murasaki {

// The daemon's global settings ($XDG_CONFIG_HOME/murasaki/settings.json): webFpsLimit (int, see kMinWebFpsLimit
// to kMaxWebFpsLimit, default kDefaultWebFpsLimit) pauseWhenCovered (bool, default kDefaultPauseWhenCovered)
// presenter (string: auto, layer-shell or plasma; default kDefaultPresenter) and sceneEngine (string, see
// isValidSceneEngine(); default kDefaultSceneEngine). The pause rules: pauseOnGame (bool, default kDefaultPauseOnGame), batteryMode
// (string: off, pause or lowfps; default kDefaultBatteryMode), sandboxEngines (bool, default kDefaultSandboxEngines: run the
// scene engines inside murasaki-sandbox), requireSandbox (bool, default kDefaultRequireSandbox: refuse to start an
// engine that would run unconfined), sceneRenderScale (number: 0 for automatic, else kMinSceneRenderScale to kMaxSceneRenderScale; default
// kDefaultSceneRenderScale: the resolution the Vulkan scene engine draws at, per output pixel) and mediaRemoteArt (bool, default kDefaultMediaRemoteArt: the host downloads the https cover a media player names, for the scenes that show the cover), mediaShare (bool, default kDefaultMediaShare: scenes that use the media are told what plays, its title, artist and cover) and pauseApps (an array of at most kMaxPauseApps resource
// classes, each 1 to kMaxPauseAppLength characters after trimming; `*` and `?` match any characters; default empty), petMaxFps (int, 0 for no limit or 1 to kMaxPetFps; default kDefaultPetMaxFps: the most frames per second the desktop pets change at) and petSaveEnergy (bool, default kDefaultPetSaveEnergy: the pets are held to kPetEnergySaveFps frames a second on battery or with the power-saver profile), petsVolume (int 0..100, default kDefaultPetsVolume: the master volume of the pets' sounds), petsMuted (bool, default false) and petsMuteWhenFullscreen (bool, default true: the sounds are muted while a fullscreen window or a pauseApps window is up on the pet's output), stageHideOverFullscreen (bool, default true: a pet in the fullscreen mode is hidden while a fullscreen window is on its output), petsHideOnGame (bool, default false: while a game runs the pets on the screens a window covers are hidden, the rule of pauseOnGame) and stageMaxPixels (int kMinStageMaxPixels to kMaxStageMaxPixels, default kDefaultStageMaxPixels: the most device pixels the fullscreen mode draws in total).
class SettingsStore : public QObject
{
    Q_OBJECT

public:
    explicit SettingsStore(QString filePath, QObject *parent = nullptr);

    QString filePath() const { return m_path; }

    // A missing file means defaults. An unreadable or invalid file is renamed to <file>.bad-<UTC time> and the
    // defaults apply; if that rename fails, saving stays disabled so the user's file is never overwritten.
    // A file with any invalid value (wrong type or out of range) is quarantined whole and the defaults apply for
    // every key. Unknown keys are ignored and dropped on the next save.
    void load();

    bool shortcutsLaunch() const { return m_shortcutsLaunch; }
    long long stageMaxPixels() const { return m_stageMaxPixels; }
    // The defaults of the pet imports (importsettings.h; keys importFormat, importSizePolicy, importFps, importKeepSource, importKeepAudio, convertGifOnImport,
    // importWorkshopFiles, importWorkshopMdaa, importMaxOutputMiB, importVideoMaxSide, importVideoMaxFps).
    const ImportSettings &importSettings() const { return m_imports; }
    // automationEnabled, fireByTimelineName, triggerCooldownMs, demoCardsDismissed (automationsettings.h).
    const AutomationSettings &automationSettings() const { return m_automation; }
    // puppetMaxPixels and puppetCorePath (puppetsettings.h).
    const PuppetSettings &puppetSettings() const { return m_puppet; }

    // Every key, defaults filled in.
    QJsonObject values() const;

    // Validates all keys first (unknown key, wrong type or out of range fails with nothing changed), then applies
    // and saves. changed() fires only when a value actually differs. If saving fails (or is blocked by an
    // unreadable file), the values are rolled back, nothing is signalled and false is returned with *saveFailed set;
    // validation failures leave *saveFailed false.
    bool merge(const QJsonObject &changes, QString *error = nullptr, bool *saveFailed = nullptr);

signals:
    void changed();

private:
    bool save() const;

    QString m_path;
    int m_webFpsLimit = kDefaultWebFpsLimit;
    bool m_pauseWhenCovered = kDefaultPauseWhenCovered;
    QString m_presenter = QString(kDefaultPresenter);
    QString m_sceneEngine = QString(kDefaultSceneEngine);
    bool m_pauseOnGame = kDefaultPauseOnGame;
    QString m_batteryMode = QString(kDefaultBatteryMode);
    QStringList m_pauseApps;
    bool m_sandboxEngines = kDefaultSandboxEngines;
    bool m_requireSandbox = kDefaultRequireSandbox;
    double m_sceneRenderScale = kDefaultSceneRenderScale;
    bool m_mediaRemoteArt = kDefaultMediaRemoteArt;
    bool m_mediaShare = kDefaultMediaShare;
    bool m_shortcutsLaunch = kDefaultShortcutsLaunch;
    int m_petMaxFps = kDefaultPetMaxFps;
    bool m_petSaveEnergy = kDefaultPetSaveEnergy;
    int m_petsVolume = kDefaultPetsVolume;
    bool m_petsMuted = kDefaultPetsMuted;
    bool m_petsMuteWhenFullscreen = kDefaultPetsMuteWhenFullscreen;
    bool m_stageHideOverFullscreen = kDefaultStageHideOverFullscreen;
    bool m_petsHideOnGame = kDefaultPetsHideOnGame;
    long long m_stageMaxPixels = kDefaultStageMaxPixels;
    ImportSettings m_imports;
    AutomationSettings m_automation;
    PuppetSettings m_puppet;
    bool m_saveBlocked = false;
};

} // namespace murasaki
