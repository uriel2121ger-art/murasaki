#include "canvas.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace murasaki {

std::optional<FillMode> parseFillMode(const QString &text)
{
    if (text == u"fill")
        return FillMode::Fill;
    if (text == u"fit")
        return FillMode::Fit;
    if (text == u"stretch")
        return FillMode::Stretch;
    return std::nullopt;
}

SpanLayout spanLayout(const QList<SpanOutput> &outputs)
{
    QList<SpanOutput> usable;
    QSet<QString> seen;
    for (const SpanOutput &output : outputs) {
        if (output.logical.isEmpty() || seen.contains(output.name))
            continue;
        seen.insert(output.name);
        usable.append(output);
    }
    SpanLayout layout;
    if (usable.isEmpty())
        return layout;
    QRectF bounds = usable.first().logical;
    for (const SpanOutput &output : std::as_const(usable))
        bounds = bounds.united(output.logical);
    layout.canvas = bounds.size();
    for (const SpanOutput &output : std::as_const(usable))
        layout.viewports.insert(output.name, output.logical.translated(-bounds.topLeft()));
    return layout;
}

QRectF placeContent(const QSizeF &canvas, const QSizeF &content, FillMode fill)
{
    if (canvas.isEmpty() || content.isEmpty())
        return {};
    if (fill == FillMode::Stretch)
        return QRectF(QPointF(0, 0), canvas);
    const double ratioX = canvas.width() / content.width();
    const double ratioY = canvas.height() / content.height();
    const double scale = fill == FillMode::Fill ? std::max(ratioX, ratioY) : std::min(ratioX, ratioY);
    const QSizeF size(content.width() * scale, content.height() * scale);
    return QRectF(QPointF((canvas.width() - size.width()) / 2, (canvas.height() - size.height()) / 2), size);
}

ViewportMapping mapViewport(const QSizeF &canvas, const QRectF &viewport, const QSizeF &content, FillMode fill)
{
    ViewportMapping mapping;
    const QRectF placed = placeContent(canvas, content, fill);
    if (placed.isEmpty() || viewport.isEmpty())
        return mapping;
    const QRectF shown = placed.intersected(viewport);
    if (shown.isEmpty())
        return mapping;
    const double scaleX = placed.width() / content.width();
    const double scaleY = placed.height() / content.height();
    mapping.visible = true;
    mapping.source = QRectF((shown.x() - placed.x()) / scaleX, (shown.y() - placed.y()) / scaleY,
                            shown.width() / scaleX, shown.height() / scaleY);
    mapping.dest = shown.translated(-viewport.topLeft());
    return mapping;
}

MpvViewport mpvViewport(const QSizeF &canvas, const QRectF &viewport, const QSizeF &content, FillMode fill)
{
    MpvViewport result;
    const QRectF placed = placeContent(canvas, content, fill);
    if (placed.isEmpty() || viewport.isEmpty() || !placed.intersects(viewport))
        return result;
    result.visible = true;
    result.aspect = placed.width() / placed.height();
    // mpv fits a picture of this aspect into the window before zoom and pan.
    const bool wider = viewport.width() / viewport.height() > result.aspect;
    const double baseWidth = wider ? viewport.height() * result.aspect : viewport.width();
    result.zoom = std::log2(placed.width() / baseWidth);
    const QPointF wanted = placed.center() - viewport.topLeft();
    result.panX = (wanted.x() - viewport.width() / 2) / placed.width();
    result.panY = (wanted.y() - viewport.height() / 2) / placed.height();
    return result;
}

} // namespace murasaki
