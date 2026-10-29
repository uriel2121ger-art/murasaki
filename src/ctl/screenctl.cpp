#include "screenctl.h"

#include "ctlutil.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QtEndian>

using namespace Qt::StringLiterals;

namespace murasaki {

QStringList formatScreenWatchState(const QJsonObject &state)
{
    QStringList lines;
    const QString name = state.value(u"state"_s).toString();
    lines << u"state: %1"_s.arg(printable(name.isEmpty() ? u"off"_s : name));
    if (const QString reason = state.value(u"reason"_s).toString(); !reason.isEmpty())
        lines << u"reason: %1"_s.arg(printable(reason));
    if (name == u"watching" || name == u"asking") {
        lines << u"regions: %1"_s.arg(state.value(u"regions"_s).toInt());
        lines << u"ticks a second: %1"_s.arg(state.value(u"ticksPerSecond"_s).toInt());
    }
    if (state.value(u"paused"_s).toBool())
        lines << u"paused: yes"_s;
    for (const QJsonValue &note : state.value(u"notes"_s).toArray())
        lines << u"note: %1"_s.arg(printable(note.toString()));
    return lines;
}

QStringList formatScreenRegions(const QJsonObject &trigger, const QJsonObject &live)
{
    QStringList lines;
    const QJsonObject data = trigger.value(u"source"_s).toObject().value(u"data"_s).toObject();
    const QJsonArray regions = data.value(u"regions"_s).toArray();
    const QJsonArray scores = live.value(u"score"_s).toArray();
    const QJsonArray refs = live.value(u"refs"_s).toArray();
    for (int i = 0; i < regions.size() && i < 64; ++i) {
        const QJsonObject r = regions.at(i).toObject();
        QString score = u"-"_s;
        if (i < scores.size() && scores.at(i).isDouble() && scores.at(i).toInt() >= 0)
            score = u"%1%"_s.arg(scores.at(i).toDouble() / 10.0, 0, 'f', 1);
        QString kept = u"-"_s;
        if (i < refs.size() && refs.at(i).isDouble())
            kept = QString::number(refs.at(i).toInt());
        lines << QStringList{QString::number(i), printable(r.value(u"kind"_s).toString(u"rect"_s)), QString::number(qRound(r.value(u"x"_s).toDouble())),
                             QString::number(qRound(r.value(u"y"_s).toDouble())), QString::number(qRound(r.value(u"w"_s).toDouble())), QString::number(qRound(r.value(u"h"_s).toDouble())), kept, score}
                         .join(u'\t');
    }
    return lines;
}

bool parseScreenSteps(const QString &text, int *steps, QString *error)
{
    bool ok = false;
    const int n = text.toInt(&ok);
    if (!ok || n < 1 || n > 64 || QString::number(n) != text) {
        if (error)
            *error = u"--steps is a whole number from 1 to 64"_s;
        return false;
    }
    *steps = n;
    return true;
}

std::optional<ScreenCapture> parseScreenCapture(const QJsonObject &result, QString *error)
{
    const auto fail = [error](const QString &why) -> std::optional<ScreenCapture> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (const QString e = result.value(u"error"_s).toString(); !e.isEmpty())
        return fail(printable(e));
    const QJsonArray steps = result.value(u"steps"_s).toArray();
    if (steps.isEmpty() || steps.size() > 64)
        return fail(u"the capture holds no pictures, or more than 64"_s);
    ScreenCapture capture;
    capture.request = result.value(u"request"_s).toString().left(64);
    qsizetype total = 0;
    for (const QJsonValue &v : steps) {
        const QJsonObject step = v.toObject();
        const QByteArray png = QByteArray::fromBase64(step.value(u"png"_s).toString().toLatin1());
        static const char signature[8] = {char(0x89), 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (png.size() < 33 || png.size() > (4 << 20) || png.left(8) != QByteArrayView(signature, 8) || png.mid(12, 4) != "IHDR")
            return fail(u"a picture of the capture is not a PNG"_s);
        const quint32 w = qFromBigEndian<quint32>(png.constData() + 16), h = qFromBigEndian<quint32>(png.constData() + 20);
        if (w < 1 || h < 1 || w > 360 || h > 360)
            return fail(u"a picture of the capture is over 360 pixels"_s);
        if (step.value(u"width"_s).toInt() != int(w) || step.value(u"height"_s).toInt() != int(h))
            return fail(u"a picture of the capture does not have the size it says"_s);
        total += png.size();
        if (total > (16 << 20))
            return fail(u"the capture is too big"_s);
        capture.pngs << png;
        capture.sizes << QSize(int(w), int(h));
    }
    return capture;
}

ScreenCaptureWaiter::ScreenCaptureWaiter(const QDBusConnection &bus, const QString &service, const QString &path, const QString &interface, QObject *parent) : QObject(parent)
{
    QDBusConnection(bus).connect(service, path, interface, u"ScreenCaptured"_s, this, SLOT(onCaptured(QString, QString)));
}

void ScreenCaptureWaiter::onCaptured(const QString &request, const QString &json)
{
    if (m_results.size() >= 8)
        m_results.removeFirst();
    // 64 pictures of at most 360x360 are some tens of MiB at the very worst; the sum of what is decoded is limited again in parseScreenCapture.
    if (json.size() > (24 << 20))
        return;
    QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    object.insert(u"request"_s, request); // the signal's own argument names the request, whatever the body says
    m_results << object;
}

std::optional<QJsonObject> ScreenCaptureWaiter::wait(const QString &request, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (true) {
        for (const QJsonObject &r : std::as_const(m_results)) {
            // Only the result of the request the daemon gave us for this call: another client's (or an earlier call's) error or pictures are not ours.
            if (r.value(u"request"_s).toString() == request)
                return r;
        }
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0)
            return std::nullopt;
        QEventLoop loop;
        QTimer::singleShot(int(qMin<qint64>(left, 100)), &loop, &QEventLoop::quit);
        loop.exec();
    }
}

} // namespace murasaki
