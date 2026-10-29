#include "spansync.h"

#include <cmath>

namespace murasaki {

QList<SpanCorrection> spanVideoCorrections(const QList<SpanPlayer> &players, double toleranceSeconds)
{
    QList<SpanCorrection> fixes;
    const SpanPlayer *reference = nullptr;
    for (const SpanPlayer &player : players) {
        if (player.position < 0)
            continue;
        if (!reference) {
            reference = &player;
            continue;
        }
        // A hair of slack: positions are floating point, and "exactly the tolerance" is within it.
        if (std::abs(player.position - reference->position) > toleranceSeconds + 1e-9)
            fixes.append({player.output, reference->position});
    }
    return fixes;
}

} // namespace murasaki
