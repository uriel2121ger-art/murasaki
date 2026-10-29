#include "murasakiservice.h"
#include "murasakiserviceinternal.h"

#include "automationmanager.h"

#include "localpets.h"
#include "localpuppets.h"
#include "petaudiomanager.h"
#include "puppetchecker.h"
#include "importablefiles.h"

#include "constants.h"
#include "playlistmanager.h"
#include "shortcut/launchassigned.h"
#include "shortcut/shortcutpolicy.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int kTranscodeDebounceMs = 3000;
constexpr int kScalesGraceMs = 5000;

} // namespace

MurasakiService::MurasakiService(Library *library, AssignmentStore *assignments, SettingsStore *settings,
                                 QString status, QString assetsDir, QObject *parent)
    : QObject(parent)
    , m_library(library)
    , m_assignments(assignments)
    , m_settings(settings)
    , m_status(std::move(status))
    , m_assetsDir(std::move(assetsDir))
{
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(kTranscodeDebounceMs);
    connect(&m_syncTimer, &QTimer::timeout, this, &MurasakiService::syncTranscodes);
    m_interactCap.setSingleShot(true);
    connect(&m_interactCap, &QTimer::timeout, this, [this] { SetWallpaperInteract(false); });
    m_graceTimer.setSingleShot(true);
    m_graceTimer.setInterval(kScalesGraceMs);
    connect(&m_graceTimer, &QTimer::timeout, this, &MurasakiService::releaseStartupHold);
    connect(m_library, &Library::itemsChanged, this, &MurasakiService::ItemsChanged);
    connect(m_assignments, &AssignmentStore::changed, this, &MurasakiService::AssignmentsChanged);
    connect(m_settings, &SettingsStore::changed, this, &MurasakiService::SettingsChanged);
    // The cap on what the stages draw sizes their copies.
    connect(m_settings, &SettingsStore::changed, this, [this] {
        if (petsAvailable())
            scheduleSync();
    });
    // A changed or removed assigned item must be reloaded by the surface, which only listens to assignments.
    connect(m_library, &Library::itemsChanged, this,
            [this](const QStringList &added, const QStringList &changed, const QStringList &removed) {
                for (const QStringList *ids : {&added, &changed, &removed}) {
                    for (const QString &id : *ids) {
                        if (m_assignments->referencesItem(id) || (m_playlistManager && m_playlistManager->showsItem(id))) {
                            emit AssignmentsChanged();
                            return;
                        }
                    }
                }
            });
}

QString MurasakiService::statusFor(const SteamLocator &steam)
{
    if (steam.libraries().isEmpty())
        return u"steam-not-found"_s;
    if (steam.appInstallDir(kWallpaperEngineAppId).isEmpty())
        return u"wallpaper-engine-not-installed"_s;
    return u"ok"_s;
}

void MurasakiService::attachPets(PetStore *pets, TranscodeManager *transcodes)
{
    m_pets = pets;
    m_transcodes = transcodes;
    connect(m_pets, &PetStore::changed, this, &MurasakiService::PetsChanged);
    connect(m_pets, &PetStore::changed, this, &MurasakiService::syncOverlayUnit);
    connect(m_transcodes, &TranscodeManager::changed, this, &MurasakiService::TranscodeChanged);
    connect(m_transcodes, &TranscodeManager::changed, this, &MurasakiService::pruneBuckets);
    connect(m_library, &Library::itemsChanged, this,
            [this](const QStringList &added, const QStringList &changed, const QStringList &removed) {
                for (const QStringList *ids : {&added, &changed, &removed}) {
                    for (const QString &id : *ids) {
                        if (m_pets->referencesItem(id)) {
                            emit PetsChanged();
                            if (!m_awaitingScales)
                                autoQueueAll();
                            return;
                        }
                    }
                }
            });
    // The overlay tells the output scales right after it starts; queue what the pets need only then (or when it
    // does not come), so that a HiDPI screen is not first given the transcode of a 1x one.
    if (m_pets->pets().isEmpty()) {
        autoQueueAll();
    } else {
        m_awaitingScales = true;
        m_graceTimer.start();
    }
}

void MurasakiService::releaseStartupHold()
{
    if (!m_awaitingScales)
        return;
    m_awaitingScales = false;
    m_graceTimer.stop();
    if (petsAvailable())
        autoQueueAll();
}

void MurasakiService::attachLocalPets(LocalPetStore *localPets)
{
    m_localPets = localPets;
}

void MurasakiService::attachLocalPuppets(LocalPetStore *localPuppets, PuppetChecker *checker)
{
    m_localPuppets = localPuppets;
    m_puppetChecker = checker;
    // An answer of the checker is a change of the item: the library lists it again (ItemsChanged follows if the item differs).
    if (m_puppetChecker)
        connect(m_puppetChecker, &PuppetChecker::changed, this, [this] { m_library->rescan(); });
}

void MurasakiService::setOverlayUnit(OverlayUnit *unit)
{
    m_overlayUnit = unit;
    syncOverlayUnit();
}

void MurasakiService::syncOverlayUnit()
{
    if (!m_overlayUnit || !m_pets)
        return;
    // (The interaction with the wallpaper is the overlay's too: its catcher windows exist only while it runs.)
    const bool wanted = !m_pets->pets().isEmpty() || m_wallpaperInteract;
    // Ask for a start when the first pet appears and, while pets exist, at most once per retry interval: a start
    // that failed (the graphical session was not up yet, systemd was busy) is retried by a later change. Asking
    // on every edit would restart a crash-looping overlay right past its RestartSec.
    if (wanted) {
        if (!m_overlayWanted || !m_overlayAsked.isValid() || m_overlayAsked.elapsed() >= m_overlayRetryMs) {
            m_overlayWanted = true;
            m_overlayAsked.start();
            m_overlayUnit->start();
        }
    } else if (m_overlayWanted) {
        m_overlayWanted = false;
        m_overlayAsked.invalidate();
        m_overlayUnit->stop();
    }
}

bool MurasakiService::registerObject(QDBusConnection &bus)
{
    return bus.registerObject(QString(kDBusPath), this,
                              QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals
                                  | QDBusConnection::ExportAllProperties);
}

quint32 MurasakiService::apiVersion() const
{
    return kDBusApiVersion;
}

void MurasakiService::rejectCall(const QString &message) const
{
    if (calledFromDBus() && !m_quiet)
        sendErrorReply(QDBusError::InvalidArgs, message);
    else
        qWarning("murasakid: %s", qPrintable(message));
}

QString MurasakiService::Settings() const
{
    return QString::fromUtf8(QJsonDocument(m_settings->values()).toJson(QJsonDocument::Compact));
}

void MurasakiService::SetSettings(const QString &settingsJson)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(settingsJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        rejectCall(u"settings must be a JSON object"_s);
        return;
    }
    QString problem;
    // A Core library the user names is checked against the file system before it is stored (apply() only reads the text): it must be a regular file of the user's or root's that nobody else can write.
    if (const QJsonValue core = doc.object().value(u"puppetCorePath"_s); core.isString() && !core.toString().isEmpty()) {
        if (const QString why = PuppetSettings::coreFileProblem(core.toString()); !why.isEmpty()) {
            rejectCall(u"puppetCorePath: "_s + why);
            return;
        }
    }
    bool saveFailed = false;
    if (m_settings->merge(doc.object(), &problem, &saveFailed))
        return;
    if (!saveFailed)
        rejectCall(problem);
    else if (calledFromDBus())
        sendErrorReply(QDBusError::Failed, problem);
    else
        qWarning("murasakid: %s", qPrintable(problem));
}

} // namespace murasaki
