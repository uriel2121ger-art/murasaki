#include "sheetslicer.h"

#include "petimport.h"

#include <algorithm>

namespace murasaki::import {
namespace {

SheetLayout refuse(std::string_view name, const QString &detail = {})
{
    SheetLayout layout;
    layout.error = QString::fromUtf8(name.data(), qsizetype(name.size()));
    if (!detail.isEmpty())
        layout.error += QStringLiteral(": ") + detail;
    return layout;
}

// How many cells of `cell` pixels, `spacing` apart, fit in `length` pixels after `padding`.
long long fit(long long length, long long padding, long long cell, long long spacing)
{
    const long long room = length - padding;
    if (room < cell)
        return 0;
    return (room + spacing) / (cell + spacing);
}

} // namespace

SheetLayout sliceSheet(const SheetSpec &spec)
{
    if (spec.sheetWidth < 1 || spec.sheetHeight < 1 || spec.sheetWidth > kMaxSheetSide || spec.sheetHeight > kMaxSheetSide
        || static_cast<long long>(spec.sheetWidth) * spec.sheetHeight > kMaxSheetPixels)
        return refuse(kSheetTooLarge, QStringLiteral("%1 x %2").arg(spec.sheetWidth).arg(spec.sheetHeight));
    if (spec.frameWidth < 1 || spec.frameHeight < 1)
        return refuse(kSheetFrameIsZero);
    if (spec.frameWidth > kMaxCanvasSide || spec.frameHeight > kMaxCanvasSide)
        return refuse(kSheetFrameLargerThanSheet, QStringLiteral("a frame over %1 pixels a side").arg(kMaxCanvasSide));
    if (spec.paddingLeft < 0 || spec.paddingTop < 0 || spec.spacingHorizontal < 0 || spec.spacingVertical < 0)
        return refuse(kSheetPaddingOutside, QStringLiteral("negative"));
    if (spec.paddingLeft >= spec.sheetWidth || spec.paddingTop >= spec.sheetHeight)
        return refuse(kSheetPaddingOutside);
    if (spec.frameCount < 1 || spec.frameCount > kMaxFrames)
        return refuse(kSheetTooManyFrames, QStringLiteral("%1").arg(spec.frameCount));
    if (spec.startFrame < 1)
        return refuse(kSheetBadStart, QStringLiteral("%1").arg(spec.startFrame));

    const long long fitColumns = fit(spec.sheetWidth, spec.paddingLeft, spec.frameWidth, spec.spacingHorizontal);
    const long long fitRows = fit(spec.sheetHeight, spec.paddingTop, spec.frameHeight, spec.spacingVertical);
    if (fitColumns < 1 || fitRows < 1)
        return refuse(kSheetFrameLargerThanSheet,
                      QStringLiteral("%1 x %2 in a sheet of %3 x %4").arg(spec.frameWidth).arg(spec.frameHeight).arg(spec.sheetWidth).arg(spec.sheetHeight));
    const long long columns = spec.columns > 0 ? std::min<long long>(spec.columns, fitColumns) : fitColumns;
    const long long rows = spec.rows > 0 ? std::min<long long>(spec.rows, fitRows) : fitRows;
    const long long cells = columns * rows;
    const long long first = static_cast<long long>(spec.startFrame) - 1;
    if (first >= cells)
        return refuse(kSheetBadStart, QStringLiteral("%1 of %2 cells").arg(spec.startFrame).arg(cells));
    if (first + spec.frameCount > cells)
        return refuse(kSheetTooFewCells, QStringLiteral("asked for %1 from frame %2, the sheet holds %3").arg(spec.frameCount).arg(spec.startFrame).arg(cells - first));

    SheetLayout layout;
    layout.columns = int(columns);
    layout.rows = int(rows);
    layout.cells.reserve(spec.frameCount);
    for (int n = 0; n < spec.frameCount; ++n) {
        const long long index = first + n;
        const long long column = index % columns;
        const long long row = index / columns;
        layout.cells.append({int(spec.paddingLeft + column * (spec.frameWidth + static_cast<long long>(spec.spacingHorizontal))),
                             int(spec.paddingTop + row * (spec.frameHeight + static_cast<long long>(spec.spacingVertical)))});
    }
    return layout;
}

} // namespace murasaki::import
