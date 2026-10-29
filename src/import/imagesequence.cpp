#include "imagesequence.h"

#include "petimport.h"

#include <algorithm>

namespace murasaki::import {

namespace {

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

} // namespace

SequencePlan planSequence(const QList<ImageDim> &sizes, SizePolicy policy)
{
    SequencePlan plan;
    if (sizes.size() < 2) {
        plan.error = named(kImagesFewer);
        return plan;
    }
    if (sizes.size() > kMaxFrames) {
        plan.error = named(kImagesTooMany, QStringLiteral("%1 images, at most %2").arg(sizes.size()).arg(kMaxFrames));
        return plan;
    }
    for (int i = 0; i < sizes.size(); ++i) {
        const ImageDim &d = sizes.at(i);
        if (d.width < 1 || d.height < 1 || d.width > kMaxCanvasSide || d.height > kMaxCanvasSide) {
            plan.error = named(kNotAnImage, QStringLiteral("image %1 is %2 x %3").arg(i + 1).arg(d.width).arg(d.height));
            return plan;
        }
    }
    const ImageDim first = sizes.first();
    if (policy == SizePolicy::Fit) {
        plan.canvasWidth = std::max_element(sizes.begin(), sizes.end(), [](const ImageDim &a, const ImageDim &b) { return a.width < b.width; })->width;
        plan.canvasHeight = std::max_element(sizes.begin(), sizes.end(), [](const ImageDim &a, const ImageDim &b) { return a.height < b.height; })->height;
    } else {
        plan.canvasWidth = first.width;
        plan.canvasHeight = first.height;
    }
    plan.placements.reserve(sizes.size());
    for (int i = 0; i < sizes.size(); ++i) {
        const ImageDim &d = sizes.at(i);
        switch (policy) {
        case SizePolicy::First:
            if (!(d == first)) {
                plan.placements.clear();
                plan.error = named(kImagesSizesDiffer, QStringLiteral("image %1 is %2 x %3, the first is %4 x %5").arg(i + 1).arg(d.width).arg(d.height).arg(first.width).arg(first.height));
                return plan;
            }
            plan.placements.append({0, 0, d.width, d.height});
            break;
        case SizePolicy::Fit:
            plan.placements.append({(plan.canvasWidth - d.width) / 2, (plan.canvasHeight - d.height) / 2, d.width, d.height});
            break;
        case SizePolicy::Scale:
            plan.placements.append({0, 0, plan.canvasWidth, plan.canvasHeight});
            break;
        }
    }
    return plan;
}

} // namespace murasaki::import
