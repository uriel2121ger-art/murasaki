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

double MurasakiService::scaleFor(const QString &output) const
{
    if (output != u"*"_s)
        return m_outputScales.value(output, 1.0);
    double largest = 1.0;
    for (const double scale : m_outputScales)
        largest = std::max(largest, scale);
    return largest;
}

int MurasakiService::needFor(const PetPlacement &placement, const PetVariant &variant) const
{
    const QSize native(variant.width, variant.height);
    if (placement.stage.active()) {
        // The same function the overlay sizes what it plays with (stageRequestNeed), from the same outputs (the overlay told their places and which is the primary),
        // the same cap factor and the same request: so the copy the daemon makes is the one the overlay asks for. Without the places the overlay draws the pet as a pet,
        // and so is it sized here.
        const QList<StageOutput> outputs = stageOutputList();
        if (!outputs.isEmpty()) {
            const int need = stageRequestNeed(outputs, stageRequestOf(placement, native), stageFactor(), std::max(variant.width, variant.height));
            if (need > 0)
                return need;
        }
    }
    return transcodeNeed(displayedLongestSide(placement.width, native), scaleFor(placement.output),
                         std::max(variant.width, variant.height));
}

QList<StageOutput> MurasakiService::stageOutputList() const
{
    QList<StageOutput> outputs;
    for (auto it = m_outputInfo.cbegin(); it != m_outputInfo.cend(); ++it) {
        if (!it->rect.isNull())
            outputs.append({it.key(), it->rect, it->scale, it->primary});
    }
    return outputs;
}

StageRequest MurasakiService::stageRequestOf(const PetPlacement &placement, QSize native)
{
    StageRequest request;
    request.mode = placement.stage.mode;
    request.fit = placement.stage.fit;
    request.output = placement.output;
    request.rotation = placement.rotation;
    request.native = native;
    return request;
}

// All the stages together draw at most stageMaxPixels: the fraction of the size they are drawn at, as the overlay computes it from the same outputs.
double MurasakiService::stageFactor() const
{
    if (!m_pets || !m_settings)
        return 1.0;
    const QList<StageOutput> outputs = stageOutputList();
    QList<StageRequest> requests;
    for (const PetPlacement &placement : m_pets->pets()) {
        if (!placement.stage.active())
            continue;
        const auto item = m_library->item(placement.itemId);
        if (!item)
            continue;
        for (const PetVariant &variant : item->variants) {
            if (variant.name == placement.variant) {
                requests.append(stageRequestOf(placement, QSize(variant.width, variant.height)));
                break;
            }
        }
    }
    return stageResolutionFactor(stageRequestPixels(outputs, requests), m_settings->stageMaxPixels());
}

QList<int> MurasakiService::needsFor(const QString &itemId, const PetVariant &variant) const
{
    QList<int> needs;
    if (!m_pets)
        return needs;
    for (const PetPlacement &placement : m_pets->pets()) {
        if (placement.itemId == itemId && placement.usesVariant(variant.name) && !placement.playOriginal)
            needs.append(needFor(placement, variant));
    }
    return needs;
}

bool MurasakiService::hasPlacements(const QString &itemId, const PetVariant &variant) const
{
    return !needsFor(itemId, variant).isEmpty();
}

QList<int> MurasakiService::doneUsableBy(const WallpaperItem &item, const PetVariant &variant, int fps) const
{
    QList<int> done;
    if (!m_transcodes)
        return done;
    for (const auto &copy : m_transcodes->doneCopies(sourceFor(item, variant, kTranscodeMaxSide, 0))) {
        if ((copy.playFps == 0 || fps > 0) && !done.contains(copy.bucket)) // a retimed copy serves a pet at any fixed rate
            done.append(copy.bucket);
    }
    return done;
}

std::optional<int> MurasakiService::unservedBucket(const WallpaperItem &item, const PetVariant &variant) const
{
    int biggest = 0;
    if (!m_pets)
        return std::nullopt;
    for (const PetPlacement &placement : m_pets->pets()) {
        if (placement.itemId != item.id || !placement.usesVariant(variant.name) || placement.playOriginal)
            continue;
        const int need = needFor(placement, variant);
        const auto best = pickDoneBucket(doneUsableBy(item, variant, placement.fps), need);
        if (!best || *best < need) // a bigger copy serves a smaller pet (scaled down); a smaller one does not
            biggest = std::max(biggest, need);
    }
    if (biggest == 0)
        return std::nullopt;
    return bucketForNeed(biggest);
}

// The copy a placement plays when one is done (so the GUI shows "ready" for the copy that is really used, a bigger one included), else the state
// of the conversion that would serve it.
TranscodeStatus MurasakiService::reportedTranscode(const WallpaperItem &item, const PetVariant &variant) const
{
    const int fps = reportedFps(item, variant);
    const QList<int> needs = needsFor(item.id, variant);
    const QList<int> usable = doneUsableBy(item, variant, fps);
    std::optional<int> best;
    if (needs.isEmpty()) {
        // With no pet to serve, any finished copy is "ready": the biggest is what a pet placed now would get first.
        if (!usable.isEmpty())
            best = *std::max_element(usable.cbegin(), usable.cend());
    } else {
        const int need = *std::max_element(needs.cbegin(), needs.cend());
        best = pickDoneBucket(usable, need);
        if (best && *best < need)
            best.reset();
    }
    if (best) {
        // The copy of the pet's own rate if there is one, then the one at the file's timing, then any retimed one.
        const auto copies = m_transcodes->doneCopies(sourceFor(item, variant, kTranscodeMaxSide, 0));
        for (const int rank : {0, 1, 2}) {
            for (const auto &copy : copies) {
                const int copyRank = copy.playFps == fps && fps > 0 ? 0 : copy.playFps == 0 ? 1 : 2;
                if (copy.bucket == *best && copyRank == rank)
                    return m_transcodes->status(sourceFor(item, variant, copy.bucket, copy.playFps));
            }
        }
    }
    return m_transcodes->status(sourceFor(item, variant, wantedBucket(item.id, variant), 0));
}

int MurasakiService::reportedFps(const WallpaperItem &item, const PetVariant &variant) const
{
    if (m_pets) {
        for (const PetPlacement &placement : m_pets->pets()) {
            if (placement.itemId == item.id && placement.variant == variant.name && !placement.playOriginal)
                return placement.fps;
        }
    }
    return defaultPetFps(item.id);
}

int MurasakiService::wantedBucket(const QString &itemId, const PetVariant &variant) const
{
    const QList<int> needs = needsFor(itemId, variant);
    if (needs.isEmpty())
        return kTranscodeMaxSide;
    return bucketForNeed(*std::max_element(needs.cbegin(), needs.cend()));
}

TranscodeSource MurasakiService::sourceFor(const WallpaperItem &item, const PetVariant &variant, int bucket,
                                           int playFps) const
{
    return {item.id, variant.name, variant.file, variant.fps, variant.frames, bucket, playFps};
}

void MurasakiService::autoQueue(const WallpaperItem &item, const QString &variantName)
{
    if (!m_transcodes || !item.format.isEmpty())
        return;
    for (const PetVariant &variant : item.variants) {
        if (variant.name != variantName || !variant.isHeavy())
            continue;
        if (const auto bucket = unservedBucket(item, variant))
            m_transcodes->request(sourceFor(item, variant, *bucket, 0), false);
    }
}

void MurasakiService::autoQueueAll()
{
    for (const PetPlacement &placement : m_pets->pets()) {
        if (placement.playOriginal)
            continue;
        if (const auto item = m_library->item(placement.itemId)) {
            for (const QString &name : placement.variantsUsed()) // the states' variants are converted like the selected one
                autoQueue(*item, name);
        }
    }
}

QList<TranscodeManager::DoneCopy> MurasakiService::keptCopies(const WallpaperItem &item, const PetVariant &variant) const
{
    QList<TranscodeManager::DoneCopy> kept;
    if (!m_transcodes)
        return kept;
    const QList<TranscodeManager::DoneCopy> copies = m_transcodes->doneCopies(sourceFor(item, variant, kTranscodeMaxSide, 0));
    QList<int> own; // at the file's timing, newest first
    for (const auto &copy : copies) {
        if (copy.playFps == 0)
            own.append(copy.bucket);
    }
    // A copy bigger than any placement of the variant needs (a stage that was turned off) is not kept for the day it comes back: it takes minutes to make but
    // hundreds of megabytes to keep. Pets alone never need more than the biggest pet bucket.
    int cap = kTranscodeMaxSide;
    for (const int need : needsFor(item.id, variant))
        cap = std::max(cap, bucketForNeed(need));
    own.removeIf([cap](int bucket) { return bucket > cap; });
    if (!hasPlacements(item.id, variant)) {
        // Nothing shows it: the biggest copy at the file's timing and the newest others stay, up to the limit.
        for (const int bucket : bucketsToKeep(own, {}))
            kept.append({bucket, 0});
        for (const auto &copy : copies) {
            if (kept.size() >= kMaxBucketsPerVariant)
                break;
            if (copy.playFps > 0)
                kept.append(copy);
        }
        return kept;
    }
    for (const int bucket : bucketsToKeep(own, needsFor(item.id, variant)))
        kept.append({bucket, 0});
    // A copy retimed to a uniform rate (made before copies stopped depending on it) serves a pet at any fixed rate, so it stays for as long as it
    // is the only kind of copy that is as big as such a pet needs: once one at the file's timing is, it goes. The one of exactly the pet's rate
    // is preferred among the same size.
    for (const PetPlacement &placement : m_pets->pets()) {
        if (placement.itemId != item.id || !placement.usesVariant(variant.name) || placement.playOriginal || placement.fps == 0)
            continue;
        const int need = needFor(placement, variant);
        if (pickDoneBucket(own, need).value_or(0) >= need)
            continue;
        std::optional<TranscodeManager::DoneCopy> best;
        for (const auto &copy : copies) {
            if (copy.playFps == 0 || copy.bucket < need)
                continue;
            const bool better = !best || copy.bucket < best->bucket
                || (copy.bucket == best->bucket && copy.playFps == placement.fps && best->playFps != placement.fps);
            if (better)
                best = copy;
        }
        if (best && std::none_of(kept.cbegin(), kept.cend(), [&](const auto &c) { return c.bucket == best->bucket && c.playFps == best->playFps; }))
            kept.append(*best);
    }
    return kept;
}

void MurasakiService::pruneBuckets(const QString &itemId)
{
    if (!m_transcodes || !m_pets || m_pruning)
        return;
    const auto item = m_library->item(itemId);
    if (!item || item->kind != ItemKind::Pet)
        return;
    m_pruning = true; // removing a file announces a change, which comes back here
    for (const PetVariant &variant : item->variants) {
        const auto kept = keptCopies(*item, variant);
        for (const auto &copy : m_transcodes->doneCopies(sourceFor(*item, variant, kTranscodeMaxSide, 0))) {
            const bool keep = std::any_of(kept.cbegin(), kept.cend(), [&](const TranscodeManager::DoneCopy &other) {
                return other.bucket == copy.bucket && other.playFps == copy.playFps;
            });
            if (!keep)
                m_transcodes->removeDone(sourceFor(*item, variant, copy.bucket, copy.playFps));
        }
    }
    m_pruning = false;
}

void MurasakiService::scheduleSync()
{
    m_syncTimer.start();
}

void MurasakiService::cancelUnneededQueue()
{
    // A conversion that waits for its turn at a size no placement needs any more (the pet was resized again, or a finished copy now
    // serves it) is dropped; the one that runs is left to finish.
    for (const TranscodeSource &queued : m_transcodes->queued()) {
        const auto item = m_library->item(queued.itemId);
        bool needed = false;
        if (item) {
            for (const PetVariant &variant : item->variants) {
                if (variant.name != queued.variant)
                    continue;
                // Nothing shows the variant: only the explicit request for the biggest size can be waiting.
                needed = !hasPlacements(item->id, variant) ? queued.bucket == kTranscodeMaxSide
                                                           : queued.playFps == 0 && unservedBucket(*item, variant) == queued.bucket;
            }
        }
        if (!needed)
            m_transcodes->cancelQueued(queued);
    }
}

void MurasakiService::syncTranscodes()
{
    if (!petsAvailable())
        return;
    cancelUnneededQueue();
    autoQueueAll();
    QSet<QString> items;
    for (const PetPlacement &placement : m_pets->pets())
        items.insert(placement.itemId);
    for (const QString &itemId : std::as_const(items))
        pruneBuckets(itemId);
}

QList<TranscodeSource> MurasakiService::expectedTranscodes() const
{
    QList<TranscodeSource> sources;
    for (const WallpaperItem &item : m_library->items()) {
        if (item.kind != ItemKind::Pet)
            continue;
        for (const PetVariant &variant : item.variants) {
            for (const auto &copy : keptCopies(item, variant))
                sources.append(sourceFor(item, variant, copy.bucket, copy.playFps));
        }
    }
    return sources;
}

void MurasakiService::SetOutputScales(const QString &scalesJson)
{
    QString problem;
    const auto info = parseOutputInfo(scalesJson, &problem);
    if (!info) {
        rejectCall(problem);
        return;
    }
    QHash<QString, double> scales;
    for (auto it = info->cbegin(); it != info->cend(); ++it)
        scales.insert(it.key(), it->scale);
    const bool holding = m_awaitingScales;
    const bool changed = scales != m_outputScales || *info != m_outputInfo;
    m_outputScales = scales;
    m_outputInfo = *info;
    if (holding)
        releaseStartupHold(); // the first answer: queue at once, nothing has been converted at the wrong size
    else if (changed && petsAvailable())
        scheduleSync();
}

QString MurasakiService::TranscodeState(const QString &itemId) const
{
    if (!petsAvailable()) {
        rejectCall(u"pets are not available"_s);
        return {};
    }
    const auto item = m_library->item(itemId);
    if (!item || item->kind != ItemKind::Pet) {
        rejectCall(u"Unknown pet item: "_s + itemId.left(200));
        return {};
    }
    const QJsonObject reply{{u"itemId"_s, itemId}, {u"variants"_s, variantStates(*item)}};
    return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));
}

void MurasakiService::RequestTranscode(const QString &itemId, const QString &variant)
{
    if (!petsAvailable()) {
        rejectCall(u"pets are not available"_s);
        return;
    }
    const auto item = m_library->item(itemId);
    if (!item || item->kind != ItemKind::Pet) {
        rejectCall(u"Unknown pet item: "_s + itemId.left(200));
        return;
    }
    bool found = false;
    for (const PetVariant &candidate : item->variants) {
        if (!variant.isEmpty() && candidate.name != variant)
            continue; // an empty variant means every variant of the item
        found = true;
        if (!hasPlacements(item->id, candidate))
            m_transcodes->request(sourceFor(*item, candidate, wantedBucket(item->id, candidate), 0));
        else if (const auto bucket = unservedBucket(*item, candidate))
            m_transcodes->request(sourceFor(*item, candidate, *bucket, 0));
    }
    if (!found)
        rejectCall(u"Unknown variant: "_s + variant.left(200));
}

} // namespace murasaki
