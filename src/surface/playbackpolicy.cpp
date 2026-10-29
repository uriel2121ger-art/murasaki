#include "playbackpolicy.h"

#include <algorithm>
#include <utility>

namespace murasaki {

QHash<QString, PlaybackDecision> decidePlayback(const QList<PlaybackInput> &inputs, bool pauseWhenCovered)
{
    QHash<QString, PlaybackDecision> decisions;
    QHash<QString, QString> ownerByItem;
    for (const PlaybackInput &input : inputs) {
        decisions.insert(input.output, PlaybackDecision{pauseWhenCovered && input.occluded, false});
        if (input.itemId.isEmpty() || (pauseWhenCovered && input.occluded) || input.muted)
            continue;
        const auto owner = ownerByItem.constFind(input.itemId);
        // Plain string order (DP-10 < DP-2) is deterministic on purpose; it need not look natural.
        if (owner == ownerByItem.cend() || input.output < *owner)
            ownerByItem.insert(input.itemId, input.output);
    }
    for (const QString &output : std::as_const(ownerByItem))
        decisions[output].audible = true;
    return decisions;
}

bool outputWantsSpectrum(const QString &kind, bool paused, bool wantsAudio)
{
    return kind == u"web" && !paused && wantsAudio;
}

bool anyWantsSpectrum(const QList<SpectrumInput> &outputs)
{
    return std::any_of(outputs.cbegin(), outputs.cend(), [](const SpectrumInput &output) {
        return outputWantsSpectrum(output.kind, output.paused, output.wantsAudio);
    });
}

} // namespace murasaki
