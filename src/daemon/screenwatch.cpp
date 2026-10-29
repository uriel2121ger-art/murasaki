#include "screenwatch.h"

#include "crashledger.h"
#include "jsonfile.h"

#include <QBuffer>
#include <QFile>
#include <QDateTime>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <time.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

qint64 monotonicNowMs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The grid of a reference picture, decoded under a cap: the header is read first and a picture that says more than 360 pixels a side is not decoded at all, the decoded size is checked again.
QByteArray gridFromPng(const QByteArray &png)
{
    if (png.isEmpty())
        return {};
    QByteArray copy = png;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    const QSize declared = reader.size();
    if (!declared.isValid() || declared.width() > kMaxScreenRefSide || declared.height() > kMaxScreenRefSide)
        return {};
    QImage image = reader.read();
    if (image.isNull() || image.width() > kMaxScreenRefSide || image.height() > kMaxScreenRefSide)
        return {};
    image = image.convertToFormat(QImage::Format_RGB888);
    const GreyImage grey = greyFromPixels(image.constBits(), image.sizeInBytes(), image.width(), image.height(), image.bytesPerLine(), ScreenPixelFormat::Rgb24);
    return screenGridOf(grey);
}

QByteArray pngFromRgb(const QByteArray &rgb, int width, int height)
{
    if (width < 1 || height < 1 || rgb.size() != qsizetype(width) * height * 3)
        return {};
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y)
        std::memcpy(image.scanLine(y), rgb.constData() + qsizetype(y) * width * 3, size_t(width) * 3);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, "PNG") ? png : QByteArray();
}

// An object that may be in the middle of emitting the signal we are answering is not deleted under it.
template <typename T>
void retire(std::unique_ptr<T> &object)
{
    if (object)
        object.release()->deleteLater();
}

} // namespace

ScreenWatch::ScreenWatch(TriggerStore *triggers, SettingsStore *settings, ScreenRefStore *refs, Config config, Hooks hooks, QObject *parent)
    : QObject(parent)
    , m_triggers(triggers)
    , m_settings(settings)
    , m_refs(refs)
    , m_config(std::move(config))
    , m_hooks(std::move(hooks))
{
    for (QTimer *timer : {&m_petsTimer, &m_refreshTimer, &m_captureTimer, &m_jobTimer, &m_pendingTimer})
        timer->setSingleShot(true);
    m_pendingMs = m_config.pendingMs;
    m_jobMs = m_config.jobMs;
    m_lease = std::make_unique<BusLease>(m_config.bus);
    connect(m_lease.get(), &BusLease::lost, this, [this] { stop(u"the program that started it is gone"_s); });
    connect(&m_jobTimer, &QTimer::timeout, this, [this] { finishJob(u"the helper did not answer the capture"_s); });
    connect(&m_pendingTimer, &QTimer::timeout, this, &ScreenWatch::dropCapture);
    connect(&m_petsTimer, &QTimer::timeout, this, &ScreenWatch::sendPets);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Watching)
            return;
        if (enabledScreenTriggers().isEmpty()) {
            stop(u"no screen trigger is enabled"_s);
            return;
        }
        m_forceRefresh = false;
        releaseAll();
        sendSetup();
        emit changed();
    });
    connect(&m_captureTimer, &QTimer::timeout, this, &ScreenWatch::nextShot);
    m_wasEnabled = m_settings->automationSettings().screenTriggersEnabled;
}

ScreenWatch::~ScreenWatch()
{
    m_dying = true;
    stop(u"the daemon ends"_s);
}

qint64 ScreenWatch::now() const
{
    return m_hooks.nowMs ? m_hooks.nowMs() : monotonicNowMs();
}

bool ScreenWatch::idle() const
{
    return !m_portal && !m_host && !m_petsTimer.isActive() && !m_refreshTimer.isActive() && !m_captureTimer.isActive() && !m_jobTimer.isActive() && !m_pendingTimer.isActive()
        && m_state == State::Off;
}

QList<Trigger> ScreenWatch::enabledScreenTriggers() const
{
    QList<Trigger> out;
    for (const Trigger &t : m_triggers->triggers())
        if (t.enabled && t.source.kind == TriggerKind::Screen)
            out.append(t);
    return out;
}

ScreenWatch::Gate ScreenWatch::gate() const
{
    if (!m_settings->automationSettings().screenTriggersEnabled || !m_settings->automationSettings().enabled)
        return Gate::SettingOff;
    if (enabledScreenTriggers().isEmpty())
        return Gate::NoTrigger;
    if (m_locked)
        return Gate::Locked;
    if (m_config.helperPath.isEmpty())
        return Gate::NoHelper;
    return Gate::Ok;
}

void ScreenWatch::setState(State state, const QString &reason)
{
    if (m_state == state && m_reason == reason)
        return;
    m_state = state;
    m_reason = reason;
    if (state == State::Watching || state == State::Asking)
        m_sinceMs = QDateTime::currentMSecsSinceEpoch();
    else
        m_lease->release(); // (ended by itself: nothing left to lease)
    emit changed();
}

bool ScreenWatch::start(QString *error)
{
    const auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (m_state == State::Asking || m_state == State::Watching)
        return true;
    switch (gate()) {
    case Gate::SettingOff:
        return refuse(u"screen triggers are off: turn on screenTriggersEnabled (and automationEnabled) in the settings"_s);
    case Gate::NoTrigger:
        return refuse(u"no screen trigger is enabled"_s);
    case Gate::Locked:
        return refuse(u"the session is locked"_s);
    case Gate::NoHelper:
        return refuse(u"murasaki-screenmatch was not found"_s);
    case Gate::Ok:
        break;
    }
    m_portal = std::make_unique<ScreenCastPortal>(m_config.bus);
    connect(m_portal.get(), &ScreenCastPortal::stateChanged, this, &ScreenWatch::onPortalState);
    connect(m_portal.get(), &ScreenCastPortal::restoreToken, this, [this](const QString &token) { saveToken(token); });
    connect(m_portal.get(), &ScreenCastPortal::restoreTokenRefused, this, [this] { saveToken(QString()); });
    ScreenCastPortal::Options options;
    options.remember = m_settings->automationSettings().screenRemember;
    if (options.remember)
        options.restoreToken = QString::fromUtf8(loadToken());
    if (!options.restoreToken.isEmpty())
        saveToken(QString()); // a token is single use: the one this run announces is the one to keep
    setState(State::Asking);
    m_portal->start(options);
    return true;
}

void ScreenWatch::onPortalState()
{
    if (!m_portal)
        return;
    switch (m_portal->state()) {
    case ScreenCastPortal::State::Watching:
        startHelper();
        break;
    case ScreenCastPortal::State::Failed: {
        const QString why = m_portal->reason();
        const State next = why == kScreenPermissionRefused ? State::Refused : why == kScreenStoppedByDesktop ? State::Stopped : State::Failed;
        releaseAll();
        finishJob(u"the screen share ended"_s);
        dropCapture();
        if (m_host)
            m_host->stop();
        retire(m_host);
        retire(m_portal);
        setState(next, why);
        break;
    }
    case ScreenCastPortal::State::Asking:
    case ScreenCastPortal::State::Idle:
        break;
    }
}

void ScreenWatch::startHelper()
{
    // The user may have changed their mind while the dialog was open: nothing is started for a watch that has nothing to watch (or while locked it only waits).
    if (gate() == Gate::SettingOff || gate() == Gate::NoTrigger || gate() == Gate::NoHelper) {
        stop(u"nothing to watch any more"_s);
        return;
    }
    m_paused = m_locked;
    m_streams.clear();
    QList<unsigned> nodes;
    for (const ScreenCastPortal::Stream &s : m_portal->streams()) {
        const double scale = m_hooks.scaleOf ? m_hooks.scaleOf(s.x, s.y, s.width, s.height) : 1.0;
        m_streams.append({s.node, s.x, s.y, s.width, s.height, scale > 0 ? scale : 1.0, scale > 0});
        nodes.append(s.node);
    }
    const int fd = m_portal->takeRemoteFd();
    if (fd < 0 || nodes.isEmpty()) {
        if (fd >= 0)
            ::close(fd);
        retire(m_portal);
        setState(State::Failed, u"screen: no stream to watch"_s);
        return;
    }
    if (!m_ledger && !m_config.crashDir.isEmpty()) {
        m_ledger = std::make_unique<CrashLedger>(m_config.crashDir);
        m_ledger->load();
    }
    ScreenMatchHost::Config host;
    host.helperPath = m_config.helperPath;
    host.sandboxPath = m_config.sandboxPath;
    host.requireSandbox = m_config.requireSandbox;
    host.allowCapture = true; // (only ever used through capture(), while the user is watching; one a second at most)
    m_host = std::make_unique<ScreenMatchHost>(host, m_ledger.get());
    m_hostWhy.clear();
    ++m_hostGeneration;
    connect(m_host.get(), &ScreenMatchHost::reply, this, &ScreenWatch::onReply);
    connect(m_host.get(), &ScreenMatchHost::degraded, this, [this](const QString &why) { m_hostWhy = why; });
    // The host reports a refusal (no sandbox where one is required, a blocked helper) from inside start(): its state is read here, and only afterwards is it listened to.
    m_host->start(fd, nodes);
    ::close(fd); // (the host made its own copy)
    if (m_host->state() != ScreenMatchHost::State::Running) {
        failHelper(m_host->state());
        return;
    }
    connect(m_host.get(), &ScreenMatchHost::stateChanged, this, &ScreenWatch::onHostState);
    setState(State::Watching);
    sendSetup();
    int declared = 0;
    for (const Live &l : std::as_const(m_live))
        for (const screenmatch::PlannedRegion &r : l.planned.regions)
            declared += r.declared;
    if (m_hooks.notify)
        m_hooks.notify(u"Murasaki is watching %1 regions of your screen"_s.arg(declared));
}

void ScreenWatch::onHostState(ScreenMatchHost::State state)
{
    using S = ScreenMatchHost::State;
    if (state == S::Running || state == S::Idle)
        return;
    // (queued: the host says why in `degraded`, which comes after its state change, and the host is not deleted under its own signal)
    const quint64 generation = m_hostGeneration;
    QMetaObject::invokeMethod(this, [this, state, generation] {
        if (generation == m_hostGeneration)
            failHelper(state);
    }, Qt::QueuedConnection);
}

void ScreenWatch::failHelper(ScreenMatchHost::State state)
{
    using S = ScreenMatchHost::State;
    if (m_state != State::Watching && m_state != State::Asking)
        return;
    ++m_hostGeneration;
    releaseAll();
    finishJob(u"the screen helper stopped"_s);
    dropCapture();
    const QString why = m_hostWhy;
    retire(m_host);
    if (m_portal)
        m_portal->stop();
    retire(m_portal);
    switch (state) {
    case S::StreamLost:
        setState(State::Stopped, QString(kScreenStoppedByDesktop));
        break;
    case S::NoPipeWire:
        setState(State::Failed, u"screen: no PipeWire"_s);
        break;
    case S::Blocked:
        setState(State::Failed, u"screen: helper keeps crashing, turned off"_s);
        break;
    case S::Refused:
        setState(State::Failed, why.isEmpty() ? u"sandbox unavailable"_s : why);
        break;
    default:
        setState(State::Failed, why.isEmpty() ? u"screen: helper crashed"_s : why);
        break;
    }
}

void ScreenWatch::dropCapture()
{
    m_pending = {};
    m_pendingTimer.stop();
}

void ScreenWatch::lease(const QString &owner)
{
    if (m_state == State::Asking || m_state == State::Watching)
        m_lease->take(owner);
}

void ScreenWatch::stop(const QString &reason)
{
    m_lease->release();
    const bool had = m_state != State::Off || m_portal || m_host;
    ++m_hostGeneration;
    releaseAll();
    if (m_job)
        finishJob(u"watching stopped"_s);
    dropCapture();
    m_petsTimer.stop();
    m_refreshTimer.stop();
    m_captureTimer.stop();
    m_jobTimer.stop();
    m_paused = false;
    m_forceRefresh = false;
    if (m_host) {
        m_host->stop();
        if (m_dying)
            m_host.reset();
        else
            retire(m_host);
    }
    if (m_portal) {
        m_portal->stop();
        if (m_dying)
            m_portal.reset();
        else
            retire(m_portal);
    }
    m_live.clear();
    m_order.clear();
    m_notes.clear();
    m_streams.clear();
    m_lastRate = 0;
    if (had)
        setState(State::Off, reason);
}

void ScreenWatch::releaseAll()
{
    for (auto it = m_live.begin(); it != m_live.end(); ++it) {
        if (it->matcher.end(now()))
            emit deactivated(it.key());
        it->matcher.reset();
    }
}

QByteArray ScreenWatch::gridOf(const QString &triggerId, int region, int step) const
{
    return gridFromPng(m_refs->frame(triggerId, region, step));
}

void ScreenWatch::sendSetup()
{
    if (!m_host)
        return;
    screenmatch::PlanInput input;
    input.triggers = enabledScreenTriggers();
    input.streams = m_streams;
    input.ticksPerSecond = effectiveRate();
    input.grid = [this](const QString &id, int region, int step) { return gridOf(id, region, step); };
    input.steps = [this](const QString &id, int region) { return m_refs->steps(id, region); };
    const screenmatch::Plan plan = screenmatch::makePlan(input);
    m_notes = plan.notes;
    m_snapshot = snapshotOfTriggers();
    m_live.clear();
    m_order.clear();
    for (const screenmatch::PlannedTrigger &planned : plan.triggers) {
        Live live;
        live.planned = planned;
        live.matcher = ScreenMatcher(planned.spec.match);
        live.permille = QList<int>(planned.spec.regionCount, -1);
        m_live.insert(planned.spec.id, live);
        m_order.append(planned.spec.id);
    }
    for (const screenmatch::Command &c : plan.commands)
        m_host->send(c);
    m_lastRate = effectiveRate();
    sendPets();
    if (!m_paused) {
        screenmatch::Command start;
        start.type = screenmatch::CommandType::Start;
        m_host->send(start);
    }
}

void ScreenWatch::sendPets()
{
    if (!m_host)
        return;
    screenmatch::Command pets;
    pets.type = screenmatch::CommandType::Pets;
    if (m_settings->automationSettings().screenIgnorePets && m_hooks.petRects)
        pets.pets = screenmatch::mapPets(m_hooks.petRects(), m_streams);
    m_host->send(pets);
}

int ScreenWatch::effectiveRate() const
{
    const int wanted = std::clamp(m_settings->automationSettings().screenTicksPerSecond, 1, 16);
    return m_onBattery ? std::min(wanted, 2) : wanted;
}

void ScreenWatch::sendRate()
{
    if (!m_host)
        return;
    screenmatch::Command rate;
    rate.type = screenmatch::CommandType::Rate;
    rate.rate = effectiveRate();
    m_host->send(rate);
    m_lastRate = rate.rate;
}

void ScreenWatch::settingsChanged()
{
    const AutomationSettings s = m_settings->automationSettings();
    const bool enabled = s.screenTriggersEnabled && s.enabled;
    if (!enabled && (m_state != State::Off || m_portal || m_host)) {
        stop(u"turned off"_s);
    } else if (m_state == State::Watching) {
        if (effectiveRate() != m_lastRate)
            sendRate();
        m_petsTimer.start(m_config.petsDebounceMs); // (screenIgnorePets may have changed)
    }
    if (enabled && !m_wasEnabled && m_ledger)
        m_ledger->forget(m_config.helperPath); // the user turned it on again: a helper that was blocked may start
    m_wasEnabled = enabled;
    if (!s.screenRemember)
        saveToken(QString()); // the choice is not remembered any more: the token goes too
}

void ScreenWatch::triggersChanged()
{
    if (m_state != State::Watching && m_state != State::Asking)
        return;
    if (enabledScreenTriggers().isEmpty()) {
        stop(u"no screen trigger is enabled"_s); // (also while the dialog is open: the helper is never started with nothing to watch)
        return;
    }
    if (m_state == State::Watching && (m_forceRefresh || snapshotOfTriggers() != m_snapshot))
        m_refreshTimer.start(m_config.refreshDebounceMs); // (an edit of some other trigger does not touch the watch: a held action would flicker)
}

QByteArray ScreenWatch::snapshotOfTriggers() const
{
    QJsonArray array;
    for (const Trigger &t : enabledScreenTriggers())
        array.append(QJsonObject{{u"id"_s, t.id}, {u"source"_s, t.source.toJson()}});
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

void ScreenWatch::petsMoved()
{
    if (m_state == State::Watching && m_settings->automationSettings().screenIgnorePets && !m_petsTimer.isActive())
        m_petsTimer.start(m_config.petsDebounceMs);
}

void ScreenWatch::setLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    if (m_state != State::Watching && m_state != State::Asking)
        return;
    m_paused = locked; // (while asking only the flag: the helper starts paused if the session is locked when the dialog is answered)
    if (locked) {
        releaseAll();
        finishJob(u"the session was locked"_s);
        dropCapture();
    }
    if (m_state == State::Watching && m_host) {
        screenmatch::Command c;
        c.type = locked ? screenmatch::CommandType::Stop : screenmatch::CommandType::Start;
        m_host->send(c);
    }
    emit changed();
}

void ScreenWatch::setOnBattery(bool onBattery)
{
    if (m_onBattery == onBattery)
        return;
    m_onBattery = onBattery;
    if (m_state == State::Watching && effectiveRate() != m_lastRate)
        sendRate();
}

void ScreenWatch::startAtLogin()
{
    if (m_settings->automationSettings().screenWatchAtLogin && m_state == State::Off && gate() == Gate::Ok)
        start();
}

void ScreenWatch::onReply(const screenmatch::Reply &reply)
{
    using screenmatch::ReplyType;
    switch (reply.type) {
    case ReplyType::Scores: {
        const auto it = m_live.find(reply.trigger);
        if (it == m_live.end() || m_paused)
            return;
        Live &live = *it;
        QList<double> scores;
        live.permille = QList<int>(live.planned.spec.regionCount, -1);
        for (int i = 0; i < live.planned.spec.regionCount; ++i) {
            const int p = i < reply.permille.size() ? reply.permille.at(i) : -1;
            live.permille[i] = p;
            scores.append(p < 0 ? 0.0 : std::clamp(p / 1000.0, 0.0, 1.0)); // (a region that could not be scored never passes)
        }
        const ScreenTickResult result = live.matcher.tick(now(), scores);
        live.score = result.score;
        live.detected = result.detected;
        if (result.step != live.step) {
            live.step = result.step;
            screenmatch::Command want;
            want.type = screenmatch::CommandType::Want;
            want.trigger = reply.trigger;
            want.step = result.step;
            if (m_host)
                m_host->send(want);
        }
        if (result.event == ScreenEvent::Activated)
            emit activated(reply.trigger);
        else if (result.event == ScreenEvent::Deactivated)
            emit deactivated(reply.trigger);
        break;
    }
    case ReplyType::Tick:
        endTick();
        break;
    case ReplyType::Shot:
        if (m_job && m_job->waiting && reply.request == m_job->helperRequest) {
            m_job->shot = {reply.request, reply.width, reply.height, reply.bytes, {}, false};
            // (the whole picture is announced: one allocation, so growing it never leaves a copy of a part behind)
            m_job->shot.data.reserve(std::min<qint64>(reply.bytes, screenmatch::kMaxCaptureBytes));
        }
        break;
    case ReplyType::Chunk:
        if (m_job && m_job->waiting && reply.request == m_job->helperRequest && m_job->shot.data.size() + reply.data.size() <= screenmatch::kMaxCaptureBytes)
            m_job->shot.data.append(reply.data);
        break;
    case ReplyType::ShotEnd:
        if (m_job && m_job->waiting && reply.request == m_job->helperRequest) {
            QByteArray png = pngFromRgb(m_job->shot.data, m_job->shot.width, m_job->shot.height);
            SecureBytes::wipe(m_job->shot.data); // (the raw pixels are not needed again)
            m_job->waiting = false;
            m_jobTimer.stop();
            SecureBytes kept(png);
            SecureBytes::wipe(png);
            if (kept.isEmpty()) {
                finishJob(u"the helper's picture was not valid"_s);
                break;
            }
            m_job->pngs.push_back(std::move(kept));
            m_job->sizes.append(QSize(m_job->shot.width, m_job->shot.height));
            if (int(m_job->pngs.size()) >= m_job->steps)
                finishJob({});
            else
                m_captureTimer.start(1100); // (the helper takes one picture a second)
        }
        break;
    case ReplyType::Err:
        if (m_job && m_job->waiting && reply.word == u"capture")
            finishJob(u"the helper refused the capture: %1"_s.arg(reply.reason));
        break;
    case ReplyType::Hello:
    case ReplyType::Ok:
    case ReplyType::State:
    case ReplyType::Pong:
        break;
    }
}

// The trigger whose action does not stop with the picture (stop_on_mismatch off) ends when its action does.
void ScreenWatch::endTick()
{
    for (auto it = m_live.begin(); it != m_live.end(); ++it) {
        if (!it->matcher.active() || it->planned.spec.match.stopOnMismatch)
            continue;
        if (!m_hooks.actionRunning || !m_hooks.actionRunning(it.key())) {
            it->matcher.end(now());
            emit deactivated(it.key());
        }
    }
    // Only a change a client can see is announced (the scores and the activations), not every tick.
    QByteArray signature;
    for (const QString &id : std::as_const(m_order)) {
        const Live &live = m_live[id];
        signature += id.toUtf8() + (live.matcher.active() ? '1' : '0') + (live.detected ? '1' : '0');
        for (const int p : live.permille)
            signature += QByteArray::number(p) + ',';
    }
    if (signature != m_lastSignature) {
        m_lastSignature = signature;
        emit changed();
    }
}

QJsonObject ScreenWatch::stateJson() const
{
    static const char *names[] = {"off", "asking", "watching", "stopped", "refused", "failed"};
    int regions = 0;
    for (const Live &l : m_live)
        for (const screenmatch::PlannedRegion &r : l.planned.regions)
            regions += r.declared;
    QJsonObject o{{u"state"_s, QLatin1StringView(names[int(m_state)])},
                  {u"reason"_s, m_reason},
                  {u"regions"_s, regions},
                  {u"ticksPerSecond"_s, m_state == State::Watching ? effectiveRate() : std::clamp(m_settings->automationSettings().screenTicksPerSecond, 1, 16)},
                  {u"since"_s, m_state == State::Off ? 0.0 : double(m_sinceMs)},
                  {u"paused"_s, m_paused && m_state == State::Watching},
                  {u"notes"_s, QJsonArray::fromStringList(m_notes)}};
    return o;
}

QJsonObject ScreenWatch::triggerJson(const QString &triggerId) const
{
    const auto it = m_live.constFind(triggerId);
    if (it == m_live.constEnd())
        return {};
    QJsonArray score;
    for (const int p : it->permille)
        score.append(p);
    return {{u"score"_s, score}, {u"detected"_s, it->detected}, {u"step"_s, it->step}, {u"active"_s, it->matcher.active()}};
}

// ---------------------------------------------------------------- the explicit capture

QString ScreenWatch::capture(const QString &triggerId, int region, int steps, QString *error)
{
    const auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return QString();
    };
    if (m_state != State::Watching || !m_host)
        return refuse(u"start watching the screen first"_s);
    if (m_paused)
        return refuse(u"the session is locked"_s);
    if (m_job)
        return refuse(u"a capture is already running"_s);
    const auto trigger = m_triggers->trigger(triggerId);
    if (!trigger || trigger->source.kind != TriggerKind::Screen)
        return refuse(u"not a screen trigger: "_s + triggerId.left(80));
    const screenmatch::TriggerSpec spec = screenmatch::specOf(*trigger);
    if (region < 0 || region >= spec.regions.size())
        return refuse(u"the trigger has no such region"_s);
    if (steps < 1 || steps > screenmatch::kMaxSteps)
        return refuse(u"steps is 1 to 64"_s);
    const screenmatch::RegionSpec &r = spec.regions.at(region);
    const screenmatch::Mapped where = screenmatch::mapRectangle(r.x, r.y, r.w, r.h, m_streams);
    if (!where.onScreen)
        return refuse(QString(screenmatch::kRegionOutside));
    dropCapture(); // (an earlier capture nobody kept)
    m_job = std::make_unique<Job>();
    m_job->id = u"capture-%1"_s.arg(m_nextRequest++);
    m_job->trigger = triggerId;
    m_job->region = region;
    m_job->steps = steps;
    m_job->where = where;
    nextShot();
    return m_job ? m_job->id : QString();
}

void ScreenWatch::nextShot()
{
    if (!m_job)
        return;
    if (!m_host) {
        finishJob(u"no helper"_s);
        return;
    }
    m_jobTimer.start(m_jobMs);
    m_job->helperRequest = m_nextRequest++;
    m_job->waiting = true;
    m_job->shot = {};
    screenmatch::Command c;
    c.type = screenmatch::CommandType::Capture;
    c.request = m_job->helperRequest;
    c.stream = m_job->where.stream;
    c.x = m_job->where.x;
    c.y = m_job->where.y;
    c.w = m_job->where.w;
    c.h = m_job->where.h;
    m_host->send(c);
}

void ScreenWatch::finishJob(const QString &error)
{
    if (!m_job)
        return;
    std::unique_ptr<Job> job = std::move(m_job);
    m_captureTimer.stop();
    m_jobTimer.stop();
    QJsonObject o{{u"request"_s, job->id}, {u"trigger"_s, job->trigger}, {u"region"_s, job->region}};
    if (!error.isEmpty()) {
        o.insert(u"error"_s, error);
    } else {
        QJsonArray steps;
        for (int i = 0; i < int(job->pngs.size()); ++i) {
            QByteArray raw = job->pngs.at(i).toByteArray();
            QByteArray text = raw.toBase64();
            SecureBytes::wipe(raw);
            steps.append(QJsonObject{{u"step"_s, i}, {u"width"_s, job->sizes.at(i).width()}, {u"height"_s, job->sizes.at(i).height()}, {u"png"_s, QString::fromLatin1(text)}});
            SecureBytes::wipe(text);
        }
        o.insert(u"steps"_s, steps);
        m_pending = {job->id, job->trigger, job->region, std::move(job->pngs)};
        m_pendingTimer.start(m_pendingMs); // (nobody keeping it is the same as cancelling)
    }
    emit captured(job->id, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

bool ScreenWatch::discardCapture(const QString &request)
{
    if (m_job && m_job->id == request) {
        finishJob(u"discarded"_s);
        dropCapture();
        return true;
    }
    if (m_pending.id.isEmpty() || m_pending.id != request)
        return false;
    dropCapture();
    return true;
}

bool ScreenWatch::commitCapture(const QString &request, QString *error)
{
    const auto refuse = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (m_pending.id.isEmpty() || m_pending.id != request)
        return refuse(u"no such capture is waiting to be kept"_s);
    if (m_state != State::Watching || m_paused)
        return refuse(u"the capture is only kept while the screen is watched"_s);
    // The trigger and the region must still be there (a trigger deleted meanwhile must not get a folder nothing would remove).
    const auto trigger = m_triggers->trigger(m_pending.trigger);
    if (!trigger || trigger->source.kind != TriggerKind::Screen || m_pending.region >= std::min<qsizetype>(trigger->source.screen.value(u"regions"_s).toArray().size(), 8))
        return refuse(u"the trigger or its region is gone"_s);
    // The new pictures replace the region's old ones whole (an old step 5 must not be mixed into a new run of 3); on a failure the capture stays, to be kept again.
    if (!m_refs->removeRegion(m_pending.trigger, m_pending.region))
        return refuse(u"cannot replace the old reference pictures"_s);
    for (int i = 0; i < int(m_pending.pngs.size()); ++i) {
        QByteArray png = m_pending.pngs.at(i).toByteArray();
        const bool stored = m_refs->putFrame(m_pending.trigger, m_pending.region, i, png, error);
        SecureBytes::wipe(png);
        if (!stored)
            return false;
    }
    dropCapture();
    m_forceRefresh = true;
    m_refreshTimer.start(m_config.refreshDebounceMs);
    return true;
}

// ---------------------------------------------------------------- the restore token

QByteArray ScreenWatch::loadToken() const
{
    if (m_config.statePath.isEmpty())
        return {};
    return readJsonObjectFile(m_config.statePath).root.value(u"restoreToken"_s).toString().toUtf8();
}

void ScreenWatch::saveToken(const QString &token) const
{
    if (m_config.statePath.isEmpty())
        return;
    if (token.isEmpty() || !m_settings->automationSettings().screenRemember)
        writeJsonObjectFile(m_config.statePath, QJsonObject());
    else
        if (writeJsonObjectFile(m_config.statePath, QJsonObject{{u"restoreToken"_s, token}}))
            QFile::setPermissions(m_config.statePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner); // (a secret of the portal: for the user only)
}

} // namespace murasaki
