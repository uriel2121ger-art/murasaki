#pragma once

#include <QList>
#include <QString>

namespace murasaki {

// One output's player of a spanned video: its position in seconds, or a negative number when it has none (still
// loading, or paused: the output is left out of the comparison).
struct SpanPlayer {
    QString output;
    double position = -1;
};

struct SpanCorrection {
    QString output;
    double seconds = 0;
};

// Every output of a spanned video starts from the same place, but each is its own player and they drift. The first
// output with a position sets the time; any other that is further from it than the tolerance (50 ms by default) gets
// a seek to it. The caller orders the players (by output name) so that the reference does not change from one round
// to the next.
QList<SpanCorrection> spanVideoCorrections(const QList<SpanPlayer> &players, double toleranceSeconds = 0.05);

} // namespace murasaki
