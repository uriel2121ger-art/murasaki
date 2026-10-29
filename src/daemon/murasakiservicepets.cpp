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

bool MurasakiService::petsAvailable() const
{
    return m_pets && m_transcodes;
}

QString MurasakiService::ListPets() const
{
    QJsonArray array;
    if (m_pets) {
        for (const PetPlacement &placement : m_pets->pets())
            array.append(placement.toJson());
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString MurasakiService::AddPet(const QString &itemId, const QString &variant, const QString &output,
                                const QString &optionsJson)
{
    if (!petsAvailable()) {
        rejectCall(u"pets are not available"_s);
        return {};
    }
    const auto options = parseObject(optionsJson, true);
    if (!options) {
        rejectCall(u"options must be a JSON object"_s);
        return {};
    }
    const auto item = m_library->item(itemId);
    if (!item) {
        rejectCall(u"Unknown item: "_s + itemId.left(200));
        return {};
    }
    if (item->kind != ItemKind::Pet) {
        rejectCall(u"Only pets can be shown on the desktop: "_s + itemId.left(200));
        return {};
    }
    const PetVariant *chosen = nullptr;
    for (const PetVariant &candidate : item->variants) {
        if (variant.isEmpty() ? candidate.name == item->variants.first().name : candidate.name == variant) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        rejectCall(item->variants.isEmpty() ? u"The item has no playable animation: "_s + itemId.left(200)
                                            : u"Unknown variant: "_s + variant.left(200));
        return {};
    }

    // A package imported with "play scene N when the pet starts": that scene's animation and position, unless the caller named a variant.
    double sceneX = 100, sceneY = 100;
    if (variant.isEmpty() && isGenerated(*item)) {
        const QJsonObject mdaa = packageRecord(*item);
        const int autoScene = mdaa.value(u"autoScene"_s).toInt(-1);
        const QJsonArray scenes = mdaa.value(u"scenes"_s).toArray();
        if (autoScene >= 0 && autoScene < scenes.size()) {
            const QJsonObject scene = scenes.at(autoScene).toObject();
            for (const PetVariant &candidate : item->variants) {
                if (scene.value(u"show"_s).toBool() && candidate.name == scene.value(u"variant"_s).toString()) {
                    chosen = &candidate;
                    if (scene.contains(u"x"_s) && scene.contains(u"y"_s)) {
                        sceneX = scene.value(u"x"_s).toDouble(100);
                        sceneY = scene.value(u"y"_s).toDouble(100);
                    }
                }
            }
        }
    }
    PetPlacement base;
    base.itemId = itemId;
    base.variant = chosen->name;
    base.output = output;
    base.x = sceneX;
    base.y = sceneY;
    base.width = std::clamp(double(chosen->width), kMinPetWidth, 650.0);
    base.fps = defaultPetFps(itemId);
    QString problem;
    const auto placement = applyPlacementChanges(base, *options, &problem);
    if (!placement) {
        rejectCall(problem);
        return {};
    }
    const QString petId = m_pets->add(*placement, &problem);
    if (petId.isEmpty()) {
        rejectCall(problem);
        return {};
    }
    if (!placement->playOriginal) {
        for (const QString &name : placement->variantsUsed())
            autoQueue(*item, name);
    }
    return petId;
}

void MurasakiService::UpdatePet(const QString &petId, const QString &changesJson)
{
    if (!petsAvailable()) {
        rejectCall(u"pets are not available"_s);
        return;
    }
    const auto current = m_pets->pet(petId);
    if (!current) {
        rejectCall(u"Unknown pet: "_s + petId.left(200));
        return;
    }
    const auto changes = parseObject(changesJson, false);
    if (!changes) {
        rejectCall(u"changes must be a JSON object"_s);
        return;
    }
    QString problem;
    const auto updated = applyPlacementChanges(*current, *changes, &problem);
    if (!updated) {
        rejectCall(problem);
        return;
    }
    const auto item = m_library->item(current->itemId);
    if (updated->variant != current->variant) {
        bool known = false;
        if (item) {
            for (const PetVariant &variant : item->variants)
                known = known || variant.name == updated->variant;
        }
        if (!known) {
            rejectCall(u"Unknown variant: "_s + updated->variant.left(200));
            return;
        }
    }
    problem.clear();
    if (!m_pets->update(*updated, &problem) && !problem.isEmpty()) {
        rejectCall(problem);
        return;
    }
    if (!item)
        return;
    // A new variant, or the original no longer wanted, is queued at once. A resize or a move to another output
    // waits (debounced): a drag of the handles must not start a conversion per step.
    if (!updated->playOriginal) {
        for (const QString &name : updated->variantsUsed()) {
            if (name == updated->variant ? (updated->variant != current->variant || current->playOriginal)
                                         : (current->playOriginal || !current->usesVariant(name)))
                autoQueue(*item, name);
        }
    }
    if (updated->width != current->width || updated->output != current->output
        || updated->playOriginal != current->playOriginal || updated->fps != current->fps || updated->states != current->states)
        scheduleSync();
}

void MurasakiService::RemovePet(const QString &petId)
{
    if (!petsAvailable()) {
        rejectCall(u"pets are not available"_s);
        return;
    }
    QString problem;
    if (!m_pets->remove(petId, &problem)) {
        rejectCall(problem.isEmpty() ? u"Unknown pet: "_s + petId.left(200) : problem);
        return;
    }
    scheduleSync();
}

void MurasakiService::SetPetsHidden(bool hidden)
{
    if (m_petsHidden == hidden)
        return;
    m_petsHidden = hidden;
    emit PetsHiddenChanged(hidden);
}

void MurasakiService::SetPetsMuted(bool muted)
{
    if (m_petsMuted == muted)
        return;
    m_petsMuted = muted;
    emit PetsMutedChanged(muted);
}

void MurasakiService::attachPetAudio(PetAudioManager *audio)
{
    m_petAudio = audio;
    connect(m_petAudio, &PetAudioManager::changed, this, &MurasakiService::PetAudioChanged);
}

QStringList MurasakiService::referencedPetAudio() const
{
    QStringList ids;
    if (m_pets)
        for (const PetPlacement &p : m_pets->pets())
            if (!p.audio.asset.isEmpty())
                ids << p.audio.asset;
    return ids;
}

QString MurasakiService::ImportPetAudio(const QString &path)
{
    if (!m_petAudio) {
        rejectCall(u"pet sounds are not available"_s);
        return {};
    }
    QString problem;
    const QString id = m_petAudio->importFile(path, &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

QString MurasakiService::ImportItemAudio(const QString &itemId)
{
    if (!m_petAudio || !m_localPets || !m_imports) {
        rejectCall(u"pet sounds are not available"_s);
        return {};
    }
    // The item's file is in its job folder under the import root; the record there says whether the audio was kept and where.
    const QString root = QFileInfo(m_imports->outRoot()).canonicalFilePath();
    QString path;
    for (const QString &file : m_localPets->files()) {
        if (LocalPetImporter::itemIdFor(file) == itemId)
            path = file;
    }
    const QFileInfo info(path);
    const QDir folder = info.isDir() ? QDir(info.absoluteFilePath()) : info.absoluteDir();
    if (path.isEmpty() || root.isEmpty() || !folder.absolutePath().startsWith(root + u'/') || QFileInfo(folder.absolutePath()).dir().absolutePath() != root) {
        rejectCall(u"Not a pet imported by this daemon: "_s + itemId.left(200));
        return {};
    }
    QFile record(folder.filePath(u"source.json"_s));
    if (!record.open(QIODevice::ReadOnly) || record.size() > (1 << 20)) {
        rejectCall(u"The import has no record"_s);
        return {};
    }
    const QJsonObject audio = QJsonDocument::fromJson(record.readAll()).object().value(u"audio"_s).toObject();
    const QString kept = audio.value(u"file"_s).toString();
    if (!audio.value(u"kept"_s).toBool() || kept.isEmpty() || kept.contains(u'\0') || QDir::isAbsolutePath(kept) || kept.contains(u".."_s)) {
        rejectCall(u"This pet has no kept audio (import the video with keepAudio)"_s);
        return {};
    }
    // The kept file must be a plain file inside the job folder: its canonical path under the folder's canonical path, and not a link (a record is data a
    // job wrote, but a link in the folder could point anywhere).
    const QFileInfo soundInfo(QDir::cleanPath(folder.filePath(kept)));
    const QString canonicalFolder = QFileInfo(folder.absolutePath()).canonicalFilePath();
    const QString sound = soundInfo.canonicalFilePath();
    if (soundInfo.isSymLink() || sound.isEmpty() || canonicalFolder.isEmpty() || !sound.startsWith(canonicalFolder + u'/') || !QFileInfo(sound).isFile()) {
        rejectCall(u"The kept audio is missing"_s);
        return {};
    }
    QString problem;
    const QString id = m_petAudio->importFile(sound, &problem);
    if (id.isEmpty())
        rejectCall(problem);
    return id;
}

QString MurasakiService::PetAudioImportState(const QString &jobId) const
{
    return m_petAudio ? QString::fromUtf8(QJsonDocument(m_petAudio->job(jobId)).toJson(QJsonDocument::Compact)) : u"{}"_s;
}

void MurasakiService::CancelPetAudioImport(const QString &jobId)
{
    if (m_petAudio)
        m_petAudio->cancel(jobId);
}

QString MurasakiService::ListPetAudioAssets() const
{
    return m_petAudio ? QString::fromUtf8(QJsonDocument(m_petAudio->assets(referencedPetAudio())).toJson(QJsonDocument::Compact)) : u"[]"_s;
}

void MurasakiService::RemovePetAudioAsset(const QString &asset)
{
    if (!m_petAudio) {
        rejectCall(u"pet sounds are not available"_s);
        return;
    }
    QString problem;
    if (!m_petAudio->removeAsset(asset, referencedPetAudio(), &problem))
        rejectCall(problem);
}

int MurasakiService::CollectPetAudio()
{
    return m_petAudio ? m_petAudio->collect(referencedPetAudio(), 0) : 0;
}

void MurasakiService::RescuePets()
{
    if (!m_pets) {
        rejectCall(u"pets are not available"_s);
        return;
    }
    QString problem;
    m_pets->updateAll(
        [](PetPlacement &p) {
            p.clickThrough = false;
            p.keepAbove = true;
            p.hoverHide = false;
            p.focusFade = false;
            p.smartDodge = false;
            p.locked = false;
            p.stage.mode = StageMode::Off; // a forgotten full-screen animation that hides the desktop: the pets are small again
            p.tracking.mode = PetTrackingMode::Off; // a forgotten follower that covers what the user is doing
            if (p.opacity < 0.5)
                p.opacity = 1;
        },
        &problem);
    if (!problem.isEmpty()) {
        rejectCall(problem); // nothing was written, and the pets stay as they were (hidden too): the rescue is all or nothing
        return;
    }
    SetPetsHidden(false);
    if (m_automation)
        m_automation->rescue(); // every timeline stops and the pets that a timeline or a trigger hid or moved are back
}

void MurasakiService::SetPetsArrange(bool arrange)
{
    if (m_petsArrange == arrange)
        return;
    m_petsArrange = arrange;
    emit PetsArrangeChanged(arrange);
    if (arrange)
        SetWallpaperInteract(false); // arranging wins: the two modes both want the pointer above the desktop
}

void MurasakiService::SetWallpaperInteract(bool interact)
{
    if (interact && m_petsArrange) {
        rejectCall(u"Turn the arrange mode of the pets off first"_s);
        return;
    }
    if (m_wallpaperInteract == interact)
        return;
    m_wallpaperInteract = interact;
    if (interact)
        m_interactCap.start(m_interactCapMs);
    else
        m_interactCap.stop();
    emit WallpaperInteractChanged(interact);
    syncOverlayUnit();
}

void MurasakiService::StartScene(const QString &petId, int index)
{
    QString problem;
    if (!startScene(petId, index, &problem))
        rejectCall(problem);
}

bool MurasakiService::startScene(const QString &petId, int index, QString *problem)
{
    const auto fail = [problem](const QString &message) {
        if (problem)
            *problem = message;
        return false;
    };
    if (!petsAvailable())
        return fail(u"pets are not available"_s);
    const auto current = m_pets->pet(petId);
    if (!current)
        return fail(u"Unknown pet: "_s + petId.left(200));
    const auto item = m_library->item(current->itemId);
    const QJsonArray scenes = item && isGenerated(*item) ? packageRecord(*item).value(u"scenes"_s).toArray() : QJsonArray();
    if (index < 0 || index >= scenes.size())
        return fail(u"The item has no such scene: "_s + QString::number(index));
    const QJsonObject scene = scenes.at(index).toObject();
    if (!scene.value(u"show"_s).toBool() || scene.value(u"variant"_s).toString().isEmpty()) {
        // A scene that hides the pet hides it in the runtime layer (never stored: the rescue and a restart bring it back).
        if (!m_automation)
            return fail(u"This scene hides the pet; that needs the automation, which is not available"_s);
        m_automation->setPetHidden(petId, true);
        return true;
    }
    if (m_automation)
        m_automation->setPetHidden(petId, false); // (a scene that shows the pet also undoes an earlier hiding one)
    QJsonObject changes{{u"variant"_s, scene.value(u"variant"_s)}};
    if (scene.contains(u"x"_s) && scene.contains(u"y"_s)) {
        changes.insert(u"x"_s, scene.value(u"x"_s));
        changes.insert(u"y"_s, scene.value(u"y"_s));
    }
    UpdatePet(petId, QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact)));
    return true;
}

} // namespace murasaki
