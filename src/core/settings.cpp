#include "settings.h"

#include "automationsettings.h"
#include "importsettings.h"
#include "jsonfile.h"
#include "pettiming.h"

#include <QJsonArray>

#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

const QString kWebFpsLimit = u"webFpsLimit"_s;
const QString kPauseWhenCovered = u"pauseWhenCovered"_s;
const QString kPresenter = u"presenter"_s;
const QString kSceneEngine = u"sceneEngine"_s;
const QString kPauseOnGame = u"pauseOnGame"_s;
const QString kBatteryMode = u"batteryMode"_s;
const QString kPauseApps = u"pauseApps"_s;
const QString kSandboxEngines = u"sandboxEngines"_s;
const QString kRequireSandbox = u"requireSandbox"_s;
const QString kSceneRenderScale = u"sceneRenderScale"_s;
const QString kMediaRemoteArt = u"mediaRemoteArt"_s;
const QString kMediaShare = u"mediaShare"_s;
const QString kShortcutsLaunch = u"shortcutsLaunch"_s;
const QString kPetMaxFps = u"petMaxFps"_s;
const QString kPetSaveEnergy = u"petSaveEnergy"_s;
const QString kPetsVolume = u"petsVolume"_s;
const QString kPetsMuted = u"petsMuted"_s;
const QString kPetsMuteWhenFullscreen = u"petsMuteWhenFullscreen"_s;
const QString kStageHideOverFullscreen = u"stageHideOverFullscreen"_s;
const QString kPetsHideOnGame = u"petsHideOnGame"_s;
const QString kStageMaxPixels = u"stageMaxPixels"_s;

struct Parsed {
    int webFpsLimit;
    bool pauseWhenCovered;
    QString presenter;
    QString sceneEngine;
    bool pauseOnGame;
    QString batteryMode;
    QStringList pauseApps;
    bool sandboxEngines;
    bool requireSandbox;
    double sceneRenderScale;
    bool mediaRemoteArt;
    bool mediaShare;
    bool shortcutsLaunch;
    int petMaxFps;
    bool petSaveEnergy;
    int petsVolume;
    bool petsMuted;
    bool petsMuteWhenFullscreen;
    bool stageHideOverFullscreen;
    bool petsHideOnGame;
    long long stageMaxPixels;
    ImportSettings imports;
    AutomationSettings automation;
    PuppetSettings puppet;

    bool operator==(const Parsed &other) const = default;
};

// The one place that decides what a valid settings object is: merge() and load() share it.
bool applyChanges(Parsed &state, const QJsonObject &changes, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        if (it.key() == kWebFpsLimit) {
            // JSON has one number type, so an integral double such as 30.0 is accepted.
            if (!it.value().isDouble())
                return fail(kWebFpsLimit + u" must be an integer"_s);
            const double number = it.value().toDouble();
            if (number != std::floor(number))
                return fail(kWebFpsLimit + u" must be an integer"_s);
            if (number < kMinWebFpsLimit || number > kMaxWebFpsLimit)
                return fail(kWebFpsLimit + u" must be between %1 and %2"_s.arg(kMinWebFpsLimit).arg(kMaxWebFpsLimit));
            state.webFpsLimit = static_cast<int>(number);
        } else if (it.key() == kPauseWhenCovered) {
            if (!it.value().isBool())
                return fail(kPauseWhenCovered + u" must be a boolean"_s);
            state.pauseWhenCovered = it.value().toBool();
        } else if (it.key() == kPresenter) {
            if (!it.value().isString())
                return fail(kPresenter + u" must be a string"_s);
            const QString value = it.value().toString();
            if (!isValidPresenter(value))
                return fail(kPresenter + u" must be one of auto, layer-shell, plasma"_s);
            state.presenter = value;
        } else if (it.key() == kSceneEngine) {
            if (!it.value().isString())
                return fail(kSceneEngine + u" must be a string"_s);
            const QString value = it.value().toString();
            if (!isValidSceneEngine(value)) {
                return fail(kSceneEngine + u" must be one of auto, vulkan"_s);
            }
            state.sceneEngine = value;
        } else if (it.key() == kPauseOnGame) {
            if (!it.value().isBool())
                return fail(kPauseOnGame + u" must be a boolean"_s);
            state.pauseOnGame = it.value().toBool();
        } else if (it.key() == kBatteryMode) {
            if (!it.value().isString())
                return fail(kBatteryMode + u" must be a string"_s);
            const QString value = it.value().toString();
            if (!isValidBatteryMode(value))
                return fail(kBatteryMode + u" must be one of off, pause, lowfps"_s);
            state.batteryMode = value;
        } else if (it.key() == kPauseApps) {
            if (!it.value().isArray())
                return fail(kPauseApps + u" must be an array of strings"_s);
            const QJsonArray array = it.value().toArray();
            if (array.size() > kMaxPauseApps)
                return fail(kPauseApps + u" holds at most %1 entries"_s.arg(kMaxPauseApps));
            QStringList apps;
            for (const QJsonValue &entry : array) {
                if (!entry.isString())
                    return fail(kPauseApps + u" must be an array of strings"_s);
                const QString app = entry.toString().trimmed();
                if (app.isEmpty() || app.size() > kMaxPauseAppLength)
                    return fail(kPauseApps + u" entries must be 1 to %1 characters"_s.arg(kMaxPauseAppLength));
                apps.append(app);
            }
            state.pauseApps = apps;
        } else if (it.key() == kSandboxEngines) {
            if (!it.value().isBool())
                return fail(kSandboxEngines + u" must be a boolean"_s);
            state.sandboxEngines = it.value().toBool();
        } else if (it.key() == kRequireSandbox) {
            if (!it.value().isBool())
                return fail(kRequireSandbox + u" must be a boolean"_s);
            state.requireSandbox = it.value().toBool();
        } else if (it.key() == kSceneRenderScale) {
            if (!it.value().isDouble())
                return fail(kSceneRenderScale + u" must be a number"_s);
            const double value = it.value().toDouble();
            if (value != 0.0 && !(value >= kMinSceneRenderScale && value <= kMaxSceneRenderScale))
                return fail(kSceneRenderScale + u" must be 0 (automatic) or between %1 and %2"_s.arg(kMinSceneRenderScale).arg(kMaxSceneRenderScale));
            state.sceneRenderScale = value;
        } else if (it.key() == kMediaRemoteArt) {
            if (!it.value().isBool())
                return fail(kMediaRemoteArt + u" must be a boolean"_s);
            state.mediaRemoteArt = it.value().toBool();
        } else if (it.key() == kMediaShare) {
            if (!it.value().isBool())
                return fail(kMediaShare + u" must be a boolean"_s);
            state.mediaShare = it.value().toBool();
        } else if (it.key() == kShortcutsLaunch) {
            if (!it.value().isBool())
                return fail(kShortcutsLaunch + u" must be a boolean"_s);
            state.shortcutsLaunch = it.value().toBool();
        } else if (it.key() == kPetMaxFps) {
            if (!it.value().isDouble())
                return fail(kPetMaxFps + u" must be an integer"_s);
            const double number = it.value().toDouble();
            if (number != std::floor(number))
                return fail(kPetMaxFps + u" must be an integer"_s);
            if (number < 0 || number > kMaxPetFps)
                return fail(kPetMaxFps + u" must be 0 (no limit) or between 1 and %1"_s.arg(kMaxPetFps));
            state.petMaxFps = static_cast<int>(number);
        } else if (it.key() == kPetSaveEnergy) {
            if (!it.value().isBool())
                return fail(kPetSaveEnergy + u" must be a boolean"_s);
            state.petSaveEnergy = it.value().toBool();
        } else if (it.key() == kPetsVolume) {
            const double number = it.value().toDouble();
            if (!it.value().isDouble() || number != std::floor(number) || number < 0 || number > 100)
                return fail(kPetsVolume + u" must be an integer between 0 and 100"_s);
            state.petsVolume = static_cast<int>(number);
        } else if (it.key() == kPetsMuted) {
            if (!it.value().isBool())
                return fail(kPetsMuted + u" must be a boolean"_s);
            state.petsMuted = it.value().toBool();
        } else if (it.key() == kPetsMuteWhenFullscreen) {
            if (!it.value().isBool())
                return fail(kPetsMuteWhenFullscreen + u" must be a boolean"_s);
            state.petsMuteWhenFullscreen = it.value().toBool();
        } else if (it.key() == kStageHideOverFullscreen) {
            if (!it.value().isBool())
                return fail(kStageHideOverFullscreen + u" must be a boolean"_s);
            state.stageHideOverFullscreen = it.value().toBool();
        } else if (it.key() == kPetsHideOnGame) {
            if (!it.value().isBool())
                return fail(kPetsHideOnGame + u" must be a boolean"_s);
            state.petsHideOnGame = it.value().toBool();
        } else if (it.key() == kStageMaxPixels) {
            const double number = it.value().toDouble();
            if (!it.value().isDouble() || number != std::floor(number) || number < kMinStageMaxPixels || number > kMaxStageMaxPixels)
                return fail(kStageMaxPixels + u" must be an integer between %1 and %2"_s.arg(kMinStageMaxPixels).arg(kMaxStageMaxPixels));
            state.stageMaxPixels = static_cast<long long>(number);
        } else if (const ImportSettings::Result r = state.imports.apply(it.key(), it.value(), error); r != ImportSettings::Result::NotMine) {
            if (r == ImportSettings::Result::Error)
                return false;
        } else if (const AutomationSettings::Result a = state.automation.apply(it.key(), it.value(), error); a != AutomationSettings::Result::NotMine) {
            if (a == AutomationSettings::Result::Error)
                return false;
        } else if (const PuppetSettings::Result p = state.puppet.apply(it.key(), it.value(), error); p != PuppetSettings::Result::NotMine) {
            if (p == PuppetSettings::Result::Error)
                return false;
        } else {
            return fail(u"unknown setting: "_s + it.key().left(200));
        }
    }
    return true;
}

} // namespace

SettingsStore::SettingsStore(QString filePath, QObject *parent)
    : QObject(parent)
    , m_path(std::move(filePath))
{
}

void SettingsStore::load()
{
    m_webFpsLimit = kDefaultWebFpsLimit;
    m_pauseWhenCovered = kDefaultPauseWhenCovered;
    m_presenter = QString(kDefaultPresenter);
    m_sceneEngine = QString(kDefaultSceneEngine);
    m_pauseOnGame = kDefaultPauseOnGame;
    m_batteryMode = QString(kDefaultBatteryMode);
    m_pauseApps.clear();
    m_sandboxEngines = kDefaultSandboxEngines;
    m_requireSandbox = kDefaultRequireSandbox;
    m_sceneRenderScale = kDefaultSceneRenderScale;
    m_mediaRemoteArt = kDefaultMediaRemoteArt;
    m_mediaShare = kDefaultMediaShare;
    m_shortcutsLaunch = kDefaultShortcutsLaunch;
    m_petMaxFps = kDefaultPetMaxFps;
    m_petSaveEnergy = kDefaultPetSaveEnergy;
    m_petsVolume = kDefaultPetsVolume;
    m_petsMuted = kDefaultPetsMuted;
    m_petsMuteWhenFullscreen = kDefaultPetsMuteWhenFullscreen;
    m_stageHideOverFullscreen = kDefaultStageHideOverFullscreen;
    m_petsHideOnGame = kDefaultPetsHideOnGame;
    m_stageMaxPixels = kDefaultStageMaxPixels;
    m_imports = {};
    m_automation = {};
    m_puppet = {};
    m_saveBlocked = false;

    const JsonFileRead read = readJsonObjectFile(m_path);
    if (!read.exists)
        return;

    QString problem = read.problem;
    if (problem.isEmpty()) {
        // Unknown keys are tolerated in the file (a newer daemon may have written them) but not in merge().
        QJsonObject known;
        for (const QString &key : {kWebFpsLimit, kPauseWhenCovered, kPresenter, kSceneEngine, kPauseOnGame, kBatteryMode, kPauseApps, kSandboxEngines, kRequireSandbox, kSceneRenderScale, kMediaRemoteArt, kMediaShare, kShortcutsLaunch, kPetMaxFps, kPetSaveEnergy, kPetsVolume, kPetsMuted, kPetsMuteWhenFullscreen, kStageHideOverFullscreen, kPetsHideOnGame, kStageMaxPixels}) {
            if (read.root.contains(key))
                known.insert(key, read.root.value(key));
        }
        for (const QString &key : ImportSettings::keys() + AutomationSettings::keys() + PuppetSettings::keys()) {
            if (read.root.contains(key))
                known.insert(key, read.root.value(key));
        }
        Parsed state{m_webFpsLimit, m_pauseWhenCovered, m_presenter, m_sceneEngine, m_pauseOnGame, m_batteryMode, m_pauseApps, m_sandboxEngines, m_requireSandbox, m_sceneRenderScale, m_mediaRemoteArt, m_mediaShare, m_shortcutsLaunch, m_petMaxFps, m_petSaveEnergy, m_petsVolume, m_petsMuted, m_petsMuteWhenFullscreen, m_stageHideOverFullscreen, m_petsHideOnGame, m_stageMaxPixels, m_imports, m_automation, m_puppet};
        if (applyChanges(state, known, &problem)) {
            m_webFpsLimit = state.webFpsLimit;
            m_pauseWhenCovered = state.pauseWhenCovered;
            m_presenter = state.presenter;
            m_sceneEngine = state.sceneEngine;
            m_pauseOnGame = state.pauseOnGame;
            m_batteryMode = state.batteryMode;
            m_pauseApps = state.pauseApps;
            m_sandboxEngines = state.sandboxEngines;
            m_requireSandbox = state.requireSandbox;
            m_sceneRenderScale = state.sceneRenderScale;
            m_mediaRemoteArt = state.mediaRemoteArt;
            m_mediaShare = state.mediaShare;
            m_shortcutsLaunch = state.shortcutsLaunch;
            m_petMaxFps = state.petMaxFps;
            m_petSaveEnergy = state.petSaveEnergy;
            m_petsVolume = state.petsVolume;
            m_petsMuted = state.petsMuted;
            m_petsMuteWhenFullscreen = state.petsMuteWhenFullscreen;
            m_stageHideOverFullscreen = state.stageHideOverFullscreen;
            m_petsHideOnGame = state.petsHideOnGame;
            m_stageMaxPixels = state.stageMaxPixels;
            m_imports = state.imports;
            m_automation = state.automation;
            m_puppet = state.puppet;
            return;
        }
        problem = u"invalid value: "_s + problem;
    }
    if (!quarantineBadFile(m_path, problem, u"default settings"_s, u"settings"_s))
        m_saveBlocked = true;
}

QJsonObject SettingsStore::values() const
{
    QJsonObject all{{kWebFpsLimit, m_webFpsLimit},
                       {kPauseWhenCovered, m_pauseWhenCovered},
                       {kPresenter, m_presenter},
                       {kSceneEngine, m_sceneEngine},
                       {kPauseOnGame, m_pauseOnGame},
                       {kBatteryMode, m_batteryMode},
                       {kPauseApps, QJsonArray::fromStringList(m_pauseApps)},
                       {kSandboxEngines, m_sandboxEngines},
                       {kRequireSandbox, m_requireSandbox},
                       {kSceneRenderScale, m_sceneRenderScale},
                       {kMediaRemoteArt, m_mediaRemoteArt},
                       {kMediaShare, m_mediaShare},
                       {kShortcutsLaunch, m_shortcutsLaunch},
                       {kPetMaxFps, m_petMaxFps},
                       {kPetSaveEnergy, m_petSaveEnergy},
                       {kPetsVolume, m_petsVolume},
                       {kPetsMuted, m_petsMuted},
                       {kPetsMuteWhenFullscreen, m_petsMuteWhenFullscreen},
                       {kStageHideOverFullscreen, m_stageHideOverFullscreen},
                       {kPetsHideOnGame, m_petsHideOnGame},
                       {kStageMaxPixels, m_stageMaxPixels}};
    m_imports.write(all);
    m_automation.write(all);
    m_puppet.write(all);
    return all;
}

bool SettingsStore::merge(const QJsonObject &changes, QString *error, bool *saveFailed)
{
    if (saveFailed)
        *saveFailed = false;
    Parsed state{m_webFpsLimit, m_pauseWhenCovered, m_presenter, m_sceneEngine, m_pauseOnGame, m_batteryMode, m_pauseApps, m_sandboxEngines, m_requireSandbox, m_sceneRenderScale, m_mediaRemoteArt, m_mediaShare, m_shortcutsLaunch, m_petMaxFps, m_petSaveEnergy, m_petsVolume, m_petsMuted, m_petsMuteWhenFullscreen, m_stageHideOverFullscreen, m_petsHideOnGame, m_stageMaxPixels, m_imports, m_automation, m_puppet};
    if (!applyChanges(state, changes, error))
        return false;
    const Parsed previous{m_webFpsLimit, m_pauseWhenCovered, m_presenter, m_sceneEngine, m_pauseOnGame, m_batteryMode, m_pauseApps, m_sandboxEngines, m_requireSandbox, m_sceneRenderScale, m_mediaRemoteArt, m_mediaShare, m_shortcutsLaunch, m_petMaxFps, m_petSaveEnergy, m_petsVolume, m_petsMuted, m_petsMuteWhenFullscreen, m_stageHideOverFullscreen, m_petsHideOnGame, m_stageMaxPixels, m_imports, m_automation, m_puppet};
    if (state == previous)
        return true;
    m_webFpsLimit = state.webFpsLimit;
    m_pauseWhenCovered = state.pauseWhenCovered;
    m_presenter = state.presenter;
    m_sceneEngine = state.sceneEngine;
    m_pauseOnGame = state.pauseOnGame;
    m_batteryMode = state.batteryMode;
    m_pauseApps = state.pauseApps;
    m_sandboxEngines = state.sandboxEngines;
    m_requireSandbox = state.requireSandbox;
    m_sceneRenderScale = state.sceneRenderScale;
    m_mediaRemoteArt = state.mediaRemoteArt;
    m_mediaShare = state.mediaShare;
    m_shortcutsLaunch = state.shortcutsLaunch;
    m_petMaxFps = state.petMaxFps;
    m_petSaveEnergy = state.petSaveEnergy;
    m_petsVolume = state.petsVolume;
    m_petsMuted = state.petsMuted;
    m_petsMuteWhenFullscreen = state.petsMuteWhenFullscreen;
    m_stageHideOverFullscreen = state.stageHideOverFullscreen;
    m_petsHideOnGame = state.petsHideOnGame;
    m_stageMaxPixels = state.stageMaxPixels;
    m_imports = state.imports;
    m_automation = state.automation;
    m_puppet = state.puppet;
    if (!save()) {
        m_webFpsLimit = previous.webFpsLimit;
        m_pauseWhenCovered = previous.pauseWhenCovered;
        m_presenter = previous.presenter;
        m_sceneEngine = previous.sceneEngine;
        m_pauseOnGame = previous.pauseOnGame;
        m_batteryMode = previous.batteryMode;
        m_pauseApps = previous.pauseApps;
        m_sandboxEngines = previous.sandboxEngines;
        m_requireSandbox = previous.requireSandbox;
        m_sceneRenderScale = previous.sceneRenderScale;
        m_mediaRemoteArt = previous.mediaRemoteArt;
        m_mediaShare = previous.mediaShare;
        m_shortcutsLaunch = previous.shortcutsLaunch;
        m_petMaxFps = previous.petMaxFps;
        m_petSaveEnergy = previous.petSaveEnergy;
        m_petsVolume = previous.petsVolume;
        m_petsMuted = previous.petsMuted;
        m_petsMuteWhenFullscreen = previous.petsMuteWhenFullscreen;
        m_stageHideOverFullscreen = previous.stageHideOverFullscreen;
        m_petsHideOnGame = previous.petsHideOnGame;
        m_stageMaxPixels = previous.stageMaxPixels;
        m_imports = previous.imports;
        m_automation = previous.automation;
        m_puppet = previous.puppet;
        if (error)
            *error = u"could not save settings"_s;
        if (saveFailed)
            *saveFailed = true;
        return false;
    }
    emit changed();
    return true;
}

bool SettingsStore::save() const
{
    if (m_saveBlocked) {
        qWarning("murasakid: not saving settings over the unreadable %s", qPrintable(m_path));
        return false;
    }
    return writeJsonObjectFile(m_path, values());
}

} // namespace murasaki
