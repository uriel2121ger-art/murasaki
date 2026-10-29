// The screen trigger calls of DaemonClient (API revision 21, docs/pets/automation.md "Screen triggers"). The daemon does the work (the desktop's dialog, the sandboxed helper that reads
// the pixels, the matching); this is the page's view of it: the watch state, the start and stop buttons, and the explicit capture of a reference picture.

#include "daemonclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

using namespace Qt::StringLiterals;

namespace murasaki {

void DaemonClient::clearScreen()
{
    if (m_screenWatch.isEmpty())
        return;
    m_screenWatch.clear();
    emit screenWatchChanged();
}

void DaemonClient::refreshScreenWatch()
{
    if (!screenSupported())
        return;
    const quint64 generation = m_generation;
    m_backend->property(u"ScreenWatchState"_s, [this, generation](const DBusBackend::Reply &reply) {
        if (generation != m_generation || !reply.ok)
            return;
        const QJsonDocument doc = QJsonDocument::fromJson(reply.value.toString().toUtf8());
        if (!doc.isObject())
            return;
        const QVariantMap state = doc.object().toVariantMap();
        if (state == m_screenWatch)
            return;
        m_screenWatch = state;
        emit screenWatchChanged();
    });
    refreshTriggerStates();
}

void DaemonClient::refreshTriggerStates()
{
    if (!automationSupported())
        return;
    call(u"TriggerState"_s, {}, [this](const QVariant &value) {
        const QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8());
        if (doc.isObject() && doc.object().toVariantMap() != m_triggerStates) {
            m_triggerStates = doc.object().toVariantMap();
            emit triggerStatesChanged();
        }
    });
}

void DaemonClient::startScreenWatch()
{
    if (!screenSupported()) {
        emit error(tr("Watching the screen needs a newer Murasaki service; update and restart it."));
        return;
    }
    call(u"StartScreenWatch"_s, {}, [this](const QVariant &) { refreshScreenWatch(); }, {}, Ungated);
}

void DaemonClient::startScreenWatchLeased()
{
    if (!screenSupported()) {
        emit error(tr("Watching the screen needs a newer Murasaki service; update and restart it."));
        return;
    }
    call(
        u"StartScreenWatchLeased"_s, {}, [this](const QVariant &) { refreshScreenWatch(); },
        [this](const DBusBackend::Reply &reply) {
            if (reply.error.endsWith(u"UnknownMethod"_s))
                startScreenWatch(); // (an older build of revision 21: no lease)
            else if (reply.isInvalidArgs() || reply.isFailed())
                emit error(reply.message);
        },
        Ungated | Silent);
}

void DaemonClient::stopScreenWatch()
{
    if (screenSupported())
        call(u"StopScreenWatch"_s, {}, [this](const QVariant &) { refreshScreenWatch(); }, {}, Ungated);
}

namespace {

// What the page may show of a capture: a plain error text (shortened), or only pictures that are PNGs of at most 360 pixels a side whose header says the size the daemon said. The
// daemon is ours, but these are the pixels of the screen and the decoder is the GUI's own: a picture that is not what it says is refused whole.
QVariantMap checkedCapture(const QString &request, const QJsonObject &result)
{
    QVariantMap checked{{u"request"_s, request}};
    const auto failed = [&](const QString &why) {
        checked.insert(u"error"_s, why);
        return checked;
    };
    if (const QString e = result.value(u"error"_s).toString(); !e.isEmpty())
        return failed(e.left(200));
    const QJsonArray steps = result.value(u"steps"_s).toArray();
    if (steps.isEmpty() || steps.size() > 64)
        return failed(QStringLiteral("the capture holds no pictures, or more than 64"));
    static const char signature[8] = {char(0x89), 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    QVariantList pictures;
    for (const QJsonValue &v : steps) {
        const QJsonObject step = v.toObject();
        const QString text = step.value(u"png"_s).toString();
        if (text.size() > (4 << 20) * 4 / 3 + 8)
            return failed(QStringLiteral("a picture of the capture is too big"));
        const QByteArray png = QByteArray::fromBase64(text.toLatin1());
        if (png.size() < 33 || png.size() > (4 << 20) || png.left(8) != QByteArrayView(signature, 8) || png.mid(12, 4) != "IHDR")
            return failed(QStringLiteral("a picture of the capture is not a PNG"));
        const quint32 w = qFromBigEndian<quint32>(png.constData() + 16), h = qFromBigEndian<quint32>(png.constData() + 20);
        if (w < 1 || h < 1 || w > 360 || h > 360)
            return failed(QStringLiteral("a picture of the capture is over 360 pixels"));
        if (step.value(u"width"_s).toInt() != int(w) || step.value(u"height"_s).toInt() != int(h))
            return failed(QStringLiteral("a picture of the capture does not have the size it says"));
        pictures << QVariantMap{{u"step"_s, pictures.size()}, {u"width"_s, int(w)}, {u"height"_s, int(h)}, {u"png"_s, text}};
    }
    checked.insert(u"trigger"_s, result.value(u"trigger"_s).toString().left(64));
    checked.insert(u"region"_s, result.value(u"region"_s).toInt(-1));
    checked.insert(u"steps"_s, pictures);
    return checked;
}

} // namespace

void DaemonClient::screenCaptureArrived(const QString &request, const QJsonObject &result)
{
    if (!m_captureInFlight && m_captureRequest.isEmpty())
        return; // nobody here asked: another client's, or one that came late
    const QVariantMap checked = checkedCapture(request, result);
    if (m_captureRequest.isEmpty()) {
        m_earlyCapture = checked; // before the reply that names the request
        return;
    }
    if (request != m_captureRequest)
        return;
    m_captureRequest.clear();
    m_captureInFlight = false;
    emit screenCaptured(request, checked);
}

void DaemonClient::captureScreenReference(const QString &triggerId, int region, int steps)
{
    if (!screenSupported()) {
        emit error(tr("Watching the screen needs a newer Murasaki service; update and restart it."));
        return;
    }
    // The answer is the request's id: the pictures come with ScreenCaptured (to this client alone), which can even beat this reply. Only the last asked request is taken.
    m_captureInFlight = true;
    m_captureRequest.clear();
    m_earlyCapture.clear();
    const quint64 seq = ++m_captureSeq;
    call(u"CaptureScreenReference"_s, {triggerId, region, steps}, [this, seq](const QVariant &value) {
        const QString request = value.toString().left(64);
        if (!m_captureInFlight || seq != m_captureSeq) { // the page gave up (or asked again) before the daemon answered: it holds a capture nobody wants
            discardScreenReference(request);
            return;
        }
        if (m_earlyCapture.value(u"request"_s).toString() == request) {
            const QVariantMap early = m_earlyCapture;
            m_earlyCapture.clear();
            m_captureInFlight = false;
            emit screenCaptured(request, early);
            return;
        }
        m_captureRequest = request;
    }, [this, seq](const DBusBackend::Reply &) {
        if (seq == m_captureSeq)
            m_captureInFlight = false;
    }, Ungated);
}

void DaemonClient::discardScreenReference(const QString &request)
{
    if (screenSupported() && !request.isEmpty())
        call(u"DiscardScreenReference"_s, {request}, {}, {}, Ungated | Silent);
}

void DaemonClient::cancelScreenCapture()
{
    if (!m_captureRequest.isEmpty())
        discardScreenReference(m_captureRequest);
    m_captureInFlight = false;
    m_captureRequest.clear();
    m_earlyCapture.clear();
}

void DaemonClient::commitScreenReference(const QString &request)
{
    if (!screenSupported())
        return;
    call(u"CommitScreenReference"_s, {request}, [this, request](const QVariant &) {
        emit screenReferenceCommitted(request);
        refreshTriggers();
    }, {}, Ungated);
}

} // namespace murasaki
