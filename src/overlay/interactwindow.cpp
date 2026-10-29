#include "interactwindow.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <LayerShellQt/window.h>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

constexpr int kBorder = 3;

quint32 buttonBits(Qt::MouseButtons buttons)
{
    quint32 bits = 0;
    if (buttons.testFlag(Qt::LeftButton))
        bits |= kInteractLeft;
    if (buttons.testFlag(Qt::RightButton))
        bits |= kInteractRight;
    if (buttons.testFlag(Qt::MiddleButton))
        bits |= kInteractMiddle;
    return bits;
}

} // namespace

InteractWindow::InteractWindow(QScreen *screen)
    : m_output(screen ? screen->name() : QString())
{
    QSurfaceFormat surfaceFormat = format();
    surfaceFormat.setAlphaBufferSize(8);
    setFormat(surfaceFormat);
    setScreen(screen);
    setFlags(Qt::Window | Qt::FramelessWindowHint);
    setCursor(Qt::ArrowCursor);

    {
        if (auto *layer = LayerShellQt::Window::get(this)) {
            using LayerWindow = LayerShellQt::Window;
            m_layer = layer;
            layer->setLayer(LayerWindow::LayerTop);
            layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop | LayerWindow::AnchorBottom | LayerWindow::AnchorLeft | LayerWindow::AnchorRight));
            layer->setExclusiveZone(-1); // the whole output, panels included: a position in the window is a position on the output
            layer->setKeyboardInteractivity(LayerWindow::KeyboardInteractivityOnDemand);
            layer->setScope(u"murasaki-interact"_s);
#if MURASAKI_LAYERSHELL_HAS_SCREEN
            layer->setScreen(screen);
#endif
        }
        if (screen)
            resize(screen->size());
    }
    show();
}

void InteractWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect area(QPoint(0, 0), size());
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(area, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent(150, 95, 235, 210);
    painter.setPen(QPen(accent, kBorder));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area.adjusted(kBorder / 2, kBorder / 2, -kBorder / 2 - 1, -kBorder / 2 - 1));
    // The badge: small, in the top right corner.
    const QString text = u"Wallpaper interaction · Esc to exit"_s;
    const QFontMetrics metrics(painter.font());
    const QRect badge(width() - metrics.horizontalAdvance(text) - 28, kBorder + 6, metrics.horizontalAdvance(text) + 20, metrics.height() + 10);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 20, 50, 190));
    painter.drawRoundedRect(badge, 8, 8);
    painter.setPen(QColor(235, 225, 255));
    painter.drawText(badge, Qt::AlignCenter, text);
}

void InteractWindow::report(const QPointF &position, quint32 buttons, const QPoint &wheel)
{
    if (onPointer)
        onPointer(InteractPointer{position, QSizeF(size()), buttons, wheel});
}

void InteractWindow::mousePressEvent(QMouseEvent *event)
{
    report(event->position(), buttonBits(event->buttons()), QPoint());
}

void InteractWindow::mouseReleaseEvent(QMouseEvent *event)
{
    report(event->position(), buttonBits(event->buttons()), QPoint());
}

void InteractWindow::mouseMoveEvent(QMouseEvent *event)
{
    report(event->position(), buttonBits(event->buttons()), QPoint());
}

void InteractWindow::wheelEvent(QWheelEvent *event)
{
    report(event->position(), buttonBits(event->buttons()), event->angleDelta());
}

void InteractWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && onEscape)
        onEscape();
    else
        QRasterWindow::keyPressEvent(event);
}

} // namespace murasaki
