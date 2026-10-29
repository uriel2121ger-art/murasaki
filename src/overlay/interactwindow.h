#pragma once

#include "interactcontroller.h"

#include <QRasterWindow>

class QScreen;

namespace LayerShellQt {
class Window;
}

namespace murasaki {

// The catcher of the interaction mode on one output: a transparent window that covers it and takes the pointer above the desktop (a layer-shell
// surface on the top layer; there is none on X11), so that the clicks Plasma's desktop would take reach the wallpaper instead. It
// draws a thin border and a small badge in a corner to say the mode is on, and nothing opaque. Esc (the keyboard is asked for only by this
// window, on demand) ends the mode.
class InteractWindow : public QRasterWindow, public InteractSurface
{
    Q_OBJECT

public:
    explicit InteractWindow(QScreen *screen);

    QString output() const override { return m_output; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void report(const QPointF &position, quint32 buttons, const QPoint &wheel);

    QString m_output;
    LayerShellQt::Window *m_layer = nullptr;
};

} // namespace murasaki
