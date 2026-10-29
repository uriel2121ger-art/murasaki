#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace murasaki::import {

inline constexpr qsizetype kMaxPackJsonBytes = 4 << 20;
inline constexpr int kMaxPackTracks = 16;
inline constexpr int kMaxPackActions = 256;
inline constexpr int kMaxPackRegions = 64;
inline constexpr int kMaxPackPoints = 256;

struct TriggersPack {
    QString error;        // "mdaa: pack refused: <why>" when pack.json is not usable; the animations are still imported
    QJsonObject json;     // the strict, bounded form that goes into the record (see parseTriggersPack)
    QStringList notes;    // lines of the record's notes: sounds not followed, an animation that was not imported, ...
    bool ok() const { return error.isEmpty(); }
};

// Reads the `pack.json` (and `meta.json`, optional) of a Triggers pack into a bounded form. Only data is read: the captured frames
// (`.npz`) are never opened, paths are never followed, and a sound path is reduced to its file name and reported as not followed. Every number is range checked and every
// list capped; whatever is out of range is dropped with a note rather than trusted. `variantOfBasename` maps the lower-case base name of an imported animation member to
// its variant name; a track whose asset is not one of them keeps no variant.
//
// The form: {name, id, speedMult, animSpeedMult, event: {name, fps, frameCount}, match: {mode, threshold, agg, cooldownMs, ...}, regions: [{id, kind, x, y, w, h, points?}],
// tracks: [{name, variant, startVisible, actions: [{t, kind, x?, y?, dur, mode, sound?, volume}]}]}.
TriggersPack parseTriggersPack(const QByteArray &packJson, const QByteArray &metaJson, const QHash<QString, QString> &variantOfBasename);

} // namespace murasaki::import
