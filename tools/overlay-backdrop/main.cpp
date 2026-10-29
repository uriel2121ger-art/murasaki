// A fullscreen window of vertical color stripes, for tools/test-overlay-nested.sh: the desktop pets must show
// through their transparent pixels over it, and above it although it is fullscreen. Each press and release that reaches
// it is printed ("press X Y", "release X Y"), so the test sees a click go through a pet. Test tool, not installed.

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QRasterWindow>

#include <algorithm>
#include <cstdio>

namespace {

class Backdrop : public QRasterWindow
{
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        static const QColor stripes[] = {QColor(0xe0, 0x30, 0x30), QColor(0x30, 0xc0, 0x40), QColor(0x30, 0x60, 0xe0),
                                         QColor(0xf0, 0xd0, 0x30)};
        const int count = int(sizeof stripes / sizeof stripes[0]);
        const int stripe = std::max(1, width() / (count * 8)); // narrow stripes: any bleed shows up
        for (int x = 0, i = 0; x < width(); x += stripe, ++i)
            painter.fillRect(x, 0, stripe, height(), stripes[i % count]);
    }
    void mousePressEvent(QMouseEvent *event) override { report("press", event); }
    void mouseReleaseEvent(QMouseEvent *event) override { report("release", event); }

private:
    static void report(const char *what, const QMouseEvent *event)
    {
        const QPointF p = event->position();
        std::printf("%s %d %d\n", what, qRound(p.x()), qRound(p.y()));
        std::fflush(stdout);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("murasaki-overlay-backdrop"));
    Backdrop window;
    window.setTitle(QStringLiteral("Murasaki overlay test backdrop"));
    window.showFullScreen();
    return app.exec();
}
