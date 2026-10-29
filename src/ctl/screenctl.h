#pragma once

// `murasakictl triggers screen ...` (docs/pets/automation.md "Screen triggers", API revision 21): what the commands print and how a capture is checked before it is kept. The pure parts live
// here so a test can pin them; main.cpp does the D-Bus calls.

#include <QByteArray>
#include <QDBusConnection>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace murasaki {

// `screen status`: the lines for the daemon's ScreenWatchState ({state, reason, regions, ticksPerSecond, since, paused, notes}). Texts from the daemon are printable()'d.
QStringList formatScreenWatchState(const QJsonObject &state);

// `screen regions <trigger>`: one tab separated line per region of an entry of ListTriggers(): index, kind, x, y, w, h, the number of reference pictures and the live score (permille as
// a percentage, "-" when not watching or not scored). `live` is that trigger's `screen` object of TriggerState() ({score: [...], detected, step, active}), or empty.
QStringList formatScreenRegions(const QJsonObject &trigger, const QJsonObject &live);

// 1 to 64 (the references a region keeps), written as a plain number.
bool parseScreenSteps(const QString &text, int *steps, QString *error);

// The pictures of a ScreenCaptured result ({request, trigger, region, steps: [{step, width, height, png (base64)}]}). Refused with *error for {error}, more than 64 steps, a PNG that is not
// one, one over 4 MiB or over 360 pixels a side, or a size that disagrees with the PNG's header.
struct ScreenCapture {
    QString request;
    QList<QByteArray> pngs;
    QList<QSize> sizes;
};
std::optional<ScreenCapture> parseScreenCapture(const QJsonObject &result, QString *error);

// Waits for the ScreenCaptured signal of the daemon (subscribe BEFORE asking: the signal can beat the answer to CaptureScreenReference). Needs a running event loop only inside wait().
class ScreenCaptureWaiter : public QObject
{
    Q_OBJECT

public:
    ScreenCaptureWaiter(const QDBusConnection &bus, const QString &service, const QString &path, const QString &interface, QObject *parent = nullptr);
    // Blocks (with an event loop) until a result for `trigger`/`region` arrives, or `timeoutMs`. nullopt when it times out.
    std::optional<QJsonObject> wait(const QString &request, int timeoutMs);

private slots:
    void onCaptured(const QString &request, const QString &json);

private:
    QList<QJsonObject> m_results;
};

} // namespace murasaki
