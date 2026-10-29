#pragma once

// The reference pictures of a Triggers pack's screen triggers: which frames of a region's captured clip a
// trigger uses, and how one is kept: scaled to a longer side of at most 360 and written as a PNG. Only those frames are kept; the rest of the clip is read and dropped.

#include "npyreader.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QIODevice;

namespace murasaki::import {

inline constexpr int kScreenRefMaxSide = 360;
inline constexpr int kMaxRefFramesPerRegion = 64;
inline constexpr int kMaxRefRegions = 16;
inline constexpr int kMaxRefFramesTotal = 256;

// The pack's choice of frames (pack.json `event` and `match`).
struct ScreenRefPlan {
    bool sequencer = false;
    int refFrameIndex = 0; // relative to the clip start, clamped into the clip
    int seqLen = 8;
    int seqStep = 1;
    int startIndex = 0;    // the clip is [startIndex, endIndex] of the captured frames
    int endIndex = -1;     // -1 = to the end
};

// The plan from the bounded pack form of parseTriggersPack.
ScreenRefPlan screenRefPlanFromPack(const QJsonObject &pack);

// The frame numbers (of a clip of `frameCount` captured frames) the trigger uses: the anchor alone, or the run of seqLen frames seqStep apart from it, stopping at the clip's end
// and at kMaxRefFramesPerRegion. `shortened` is set when the run was cut. Empty when frameCount is 0.
QList<int> screenFramesUsed(const ScreenRefPlan &plan, int frameCount, bool *shortened = nullptr);

// One frame (width x height x 3 RGB) scaled by area average to a longer side of at most 360 and encoded as a PNG; empty with *error set when it cannot be.
QByteArray scaledFramePng(const uchar *rgb, int width, int height, QString *error);

// What came of one region.
struct ScreenRefFrame {
    QString file;  // relative to the job folder
    int clipIndex = 0;
};
struct ScreenRefRegion {
    int region = 0;
    QString status;           // "ready" or a name of petimport.h with its detail
    int sourceFrames = 0;     // frames in the captured array
    QList<ScreenRefFrame> frames;
    QStringList notes;
    bool ready() const { return status == QLatin1String("ready"); }
};

// Reads the .npz of region `region` from `npz`, keeps the plan's frames as PNGs under `folder`/screenrefs/region<region>/step<k>.png, and counts the bytes against *budgetLeft.
// On any refusal the region's files are removed and its status names the reason.
// `frameLimit` is how many pictures this region may still keep (the import's total cap, applied as a hard cut of the plan, with kScreenRefsOverLimit noted); `alive`, when it says
// false, stops the read inside the frames file (status `cancelled`). A clip that starts past the end of the array is not matched against some other frame: the status is
// kScreenRefsNoFrames.
ScreenRefRegion extractRegionRefs(QIODevice *npz, int region, const ScreenRefPlan &plan, const QString &folder, long long *budgetLeft, int frameLimit = kMaxRefFramesPerRegion,
                                  const std::function<bool()> &alive = {});

} // namespace murasaki::import
