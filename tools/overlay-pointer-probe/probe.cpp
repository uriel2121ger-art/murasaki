// Test tool (not installed): a layer-shell surface that behaves like a pet window, driven by commands on a FIFO, and
// that logs what the compositor tells it. Used by tools/test-overlay-pointer-nested.sh inside a private nested compositor
// to answer questions about pointer handling; the results are in docs/pets/pointer-facts.md.
//
//   murasaki-pointer-probe --fifo PATH [--layer overlay|top|bottom|background] [--x N --y N --w N --h N] [--color RRGGBB]
//
// Commands (one per line on the FIFO):
//   region none | empty | disc | rect X Y W H | grid COLS ROWS PARITY
//   layer overlay|top|bottom|background
//   expand | shrink             the window covers the whole output / goes back to x,y,w,h
//   hide | show                 paint nothing / paint the disc (the input region is not touched)
//   animate on|off             repaint at ~60 Hz and log every paint (frame log)
//   tick HZ COLS ROWS           every 1/HZ s set the "grid" region of parity 0 and 1 alternately; HZ 0 stops
//   expandonpress on|off        grow on a button press, shrink on release (the drag layout)
//   ping N                      logs "pong N"
//   quit
#include <LayerShellQt/Window>

#include <QDateTime>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QRasterWindow>
#include <QRegion>
#include <QScreen>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

static QElapsedTimer g_clock;

static void logLine(const QString &text)
{
    std::printf("t=%lld %s\n", (long long)g_clock.elapsed(), qPrintable(text));
    std::fflush(stdout);
}

class Probe : public QRasterWindow
{
public:
    Probe(QRect small, LayerShellQt::Window::Layer layer, QColor color)
        : m_small(small)
        , m_color(color)
    {
        setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        QSurfaceFormat f = format();
        f.setAlphaBufferSize(8);
        setFormat(f);
        m_layer = LayerShellQt::Window::get(this);
        m_layer->setLayer(layer);
        m_layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorLeft));
        m_layer->setExclusiveZone(-1);
        m_layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        m_layer->setScope(QStringLiteral("murasaki-spike"));
        m_layer->setMargins(QMargins(small.x(), small.y(), 0, 0));
        resize(small.size());
    }

    void command(const QString &line)
    {
        const QStringList w = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (w.isEmpty())
            return;
        const QString c = w.at(0);
        if (c == QLatin1String("region") && w.size() >= 2)
            setRegion(w.mid(1));
        else if (c == QLatin1String("layer") && w.size() == 2)
            setLayerName(w.at(1));
        else if (c == QLatin1String("expand"))
            expand();
        else if (c == QLatin1String("shrink"))
            shrink();
        else if (c == QLatin1String("hide")) {
            m_hidden = true;
            update();
            logLine(QStringLiteral("cmd hide"));
        } else if (c == QLatin1String("show")) {
            m_hidden = false;
            update();
            logLine(QStringLiteral("cmd show"));
        } else if (c == QLatin1String("unmap")) {
            hide();
            logLine(QStringLiteral("cmd unmap"));
        } else if (c == QLatin1String("map")) {
            show();
            logLine(QStringLiteral("cmd map"));
        } else if (c == QLatin1String("tick") && w.size() == 4)
            tick(w.at(1).toInt(), w.at(2).toInt(), w.at(3).toInt());
        else if (c == QLatin1String("expandonpress") && w.size() == 2)
            m_expandOnPress = w.at(1) == QLatin1String("on");
        else if (c == QLatin1String("animate") && w.size() == 2)
            animate(w.at(1) == QLatin1String("on"));
        else if (c == QLatin1String("ping") && w.size() == 2)
            logLine(QStringLiteral("pong %1").arg(w.at(1)));
        else if (c == QLatin1String("quit"))
            QGuiApplication::quit();
        else
            logLine(QStringLiteral("bad command: %1").arg(line));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_animating || size() != m_lastPaintSize) {
            logLine(QStringLiteral("paint %1x%2").arg(width()).arg(height()));
            m_lastPaintSize = size();
        }
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
        if (m_hidden)
            return;
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setRenderHint(QPainter::Antialiasing);
        // The disc is drawn where the small layout would have it, whatever the window size.
        const QRect box = m_expanded ? m_small : QRect(QPoint(0, 0), m_small.size());
        p.setBrush(m_color);
        p.setPen(QPen(Qt::black, 3));
        const int r = qMin(box.width(), box.height()) / 2 - 2;
        p.drawEllipse(box.center(), r, r);
    }

    bool event(QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::Enter: {
            const auto *ev = static_cast<QEnterEvent *>(e);
            logLine(QStringLiteral("enter %1 %2").arg(ev->position().x()).arg(ev->position().y()));
            break;
        }
        case QEvent::Leave:
            logLine(QStringLiteral("leave"));
            break;
        case QEvent::MouseMove: {
            const auto *ev = static_cast<QMouseEvent *>(e);
            logLine(QStringLiteral("move %1 %2 buttons=%3").arg(ev->position().x()).arg(ev->position().y()).arg(int(ev->buttons())));
            break;
        }
        case QEvent::MouseButtonPress: {
            const auto *ev = static_cast<QMouseEvent *>(e);
            logLine(QStringLiteral("press %1 %2").arg(ev->position().x()).arg(ev->position().y()));
            if (m_expandOnPress)
                expand();
            break;
        }
        case QEvent::MouseButtonRelease: {
            const auto *ev = static_cast<QMouseEvent *>(e);
            logLine(QStringLiteral("release %1 %2").arg(ev->position().x()).arg(ev->position().y()));
            if (m_expandOnPress)
                shrink();
            break;
        }
        case QEvent::Expose:
            logLine(QStringLiteral("expose exposed=%1 size=%2x%3").arg(isExposed()).arg(width()).arg(height()));
            break;
        case QEvent::Resize:
            logLine(QStringLiteral("resize %1x%2").arg(width()).arg(height()));
            break;
        default:
            break;
        }
        return QRasterWindow::event(e);
    }

private:
    void setRegion(const QStringList &w)
    {
        m_lastRegion = w;
        const QString kind = w.at(0);
        QRegion region;
        bool transparentForInput = false;
        const QSize size = m_expanded ? screen()->size() : m_small.size();
        const QRect box = m_expanded ? m_small : QRect(QPoint(0, 0), m_small.size());
        if (kind == QLatin1String("none")) {
            region = QRegion();
        } else if (kind == QLatin1String("empty")) {
            transparentForInput = true;
        } else if (kind == QLatin1String("disc")) {
            const int r = qMin(box.width(), box.height()) / 2 - 2;
            for (int y = -r; y <= r; ++y) {
                const int half = int(std::sqrt(double(r * r - y * y)));
                region += QRect(box.center().x() - half, box.center().y() + y, 2 * half + 1, 1);
            }
        } else if (kind == QLatin1String("rect") && w.size() == 5) {
            region = QRegion(QRect(w.at(1).toInt(), w.at(2).toInt(), w.at(3).toInt(), w.at(4).toInt()));
        } else if (kind == QLatin1String("grid") && w.size() == 4) {
            region = gridRegion(w.at(1).toInt(), w.at(2).toInt(), w.at(3).toInt());
        } else {
            logLine(QStringLiteral("bad region"));
            return;
        }
        (void)size;
        setFlag(Qt::WindowTransparentForInput, transparentForInput);
        setMask(region);
        update(); // a commit, so that the new input region is applied
        m_regionRects = region.rectCount();
        logLine(QStringLiteral("cmd region %1 rects=%2").arg(w.join(QLatin1Char(' '))).arg(m_regionRects));
    }

    // COLS x ROWS cells over the small box, a cell is "on" when (col + row) % 2 == parity: about COLS*ROWS/2 rectangles
    // (Qt merges neighbours of the same width, but a checkerboard has none to merge).
    QRegion gridRegion(int cols, int rows, int parity) const
    {
        const QRect box = m_expanded ? m_small : QRect(QPoint(0, 0), m_small.size());
        QRegion region;
        const double cw = double(box.width()) / cols, ch = double(box.height()) / rows;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (((c + r) & 1) == (parity & 1))
                    region += QRect(box.x() + int(c * cw), box.y() + int(r * ch), qMax(1, int(cw) - 0), qMax(1, int(ch) - 0));
            }
        }
        return region;
    }

    void setLayerName(const QString &name)
    {
        using L = LayerShellQt::Window;
        L::Layer layer = L::LayerOverlay;
        if (name == QLatin1String("top"))
            layer = L::LayerTop;
        else if (name == QLatin1String("bottom"))
            layer = L::LayerBottom;
        else if (name == QLatin1String("background"))
            layer = L::LayerBackground;
        m_layer->setLayer(layer);
        update();
        logLine(QStringLiteral("cmd layer %1").arg(name));
    }

    void expand()
    {
        using L = LayerShellQt::Window;
        if (m_expanded)
            return;
        m_expanded = true;
        m_layer->setAnchors(L::Anchors(L::AnchorTop | L::AnchorBottom | L::AnchorLeft | L::AnchorRight));
        m_layer->setMargins(QMargins());
        resize(screen()->size());
        if (!m_lastRegion.isEmpty())
            setRegion(m_lastRegion); // the region is in window coordinates: it moves with the disc
        update();
        logLine(QStringLiteral("cmd expand to %1x%2").arg(screen()->size().width()).arg(screen()->size().height()));
    }

    void shrink()
    {
        using L = LayerShellQt::Window;
        if (!m_expanded)
            return;
        m_expanded = false;
        m_layer->setAnchors(L::Anchors(L::AnchorTop | L::AnchorLeft));
        m_layer->setMargins(QMargins(m_small.x(), m_small.y(), 0, 0));
        resize(m_small.size());
        if (!m_lastRegion.isEmpty())
            setRegion(m_lastRegion);
        update();
        logLine(QStringLiteral("cmd shrink"));
    }

    void animate(bool on)
    {
        m_animating = on;
        m_animTimer.stop();
        if (on) {
            m_animTimer.setTimerType(Qt::PreciseTimer);
            m_animTimer.setInterval(16);
            m_animTimer.disconnect();
            connect(&m_animTimer, &QTimer::timeout, this, [this] { update(); });
            m_animTimer.start();
        }
        logLine(QStringLiteral("cmd animate %1").arg(on ? "on" : "off"));
    }

    void tick(int hz, int cols, int rows)
    {
        m_timer.stop();
        if (hz <= 0) {
            logLine(QStringLiteral("cmd tick off updates=%1").arg(m_updates));
            return;
        }
        m_updates = 0;
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(1000 / hz);
        m_timer.disconnect();
        connect(&m_timer, &QTimer::timeout, this, [this, cols, rows] {
            setFlag(Qt::WindowTransparentForInput, false);
            setMask(gridRegion(cols, rows, m_updates++ & 1));
            update();
        });
        m_timer.start();
        logLine(QStringLiteral("cmd tick %1 Hz grid %2x%3").arg(hz).arg(cols).arg(rows));
    }

    QRect m_small;
    LayerShellQt::Window *m_layer = nullptr;
    bool m_expanded = false;
    bool m_hidden = false;
    bool m_expandOnPress = false;
    int m_regionRects = 0;
    int m_updates = 0;
    QTimer m_timer;
    QTimer m_animTimer;
    QColor m_color;
    QStringList m_lastRegion;
    QSize m_lastPaintSize;
    bool m_animating = false;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    g_clock.start();
    // The wall clock at t=0, to line the log up with other logs (a KWin script prints Date.now()).
    std::printf("epoch %lld\n", (long long)QDateTime::currentMSecsSinceEpoch());
    QString fifo;
    QString layerName = QStringLiteral("overlay");
    QRect small(200, 150, 200, 200);
    QColor color(255, 140, 0);
    for (int i = 1; i + 1 < argc; i += 2) {
        const QString key = QString::fromLocal8Bit(argv[i]), value = QString::fromLocal8Bit(argv[i + 1]);
        if (key == QLatin1String("--fifo")) fifo = value;
        else if (key == QLatin1String("--layer")) layerName = value;
        else if (key == QLatin1String("--x")) small.moveLeft(value.toInt());
        else if (key == QLatin1String("--y")) small.moveTop(value.toInt());
        else if (key == QLatin1String("--w")) small.setWidth(value.toInt());
        else if (key == QLatin1String("--h")) small.setHeight(value.toInt());
        else if (key == QLatin1String("--color")) color = QColor(QStringLiteral("#") + value);
    }
    if (fifo.isEmpty()) {
        std::fprintf(stderr, "pointer-probe: --fifo PATH is required\n");
        return 2;
    }
    Probe probe(small, LayerShellQt::Window::LayerOverlay, color);
    probe.show();
    probe.command(QStringLiteral("layer %1").arg(layerName));

    const int fd = ::open(qPrintable(fifo), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::perror("pointer-probe: fifo");
        return 2;
    }
    QByteArray pending;
    QSocketNotifier notifier(fd, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, [&] {
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0)
            pending.append(buf, int(n));
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            probe.command(QString::fromLocal8Bit(pending.left(nl)));
            pending.remove(0, nl + 1);
        }
    });
    logLine(QStringLiteral("ready"));
    return app.exec();
}
