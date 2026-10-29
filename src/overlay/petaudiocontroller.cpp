#include "petaudiocontroller.h"

#include "protocol.h"
#include "sceneenginelookup.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>

#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <cmath>

namespace murasaki {

using namespace Qt::StringLiterals;

namespace {

qint64 monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

QString name(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

constexpr qint64 kLateNs = 100'000'000;       // the player drops a start later than this
constexpr qint64 kUnavailableRetryMs = 30'000;
constexpr int kFramesPerMs = kPetAudioRate / 1000;

} // namespace

// ---- the process link ---------------------------------------------------------------------------------------------

struct ProcessPetAudioLink::Impl {
    QString assets;
    QString program;
    std::unique_ptr<QProcess> process;
    QStringList queue;
    bool ready = false;
    QByteArray pending;
    bool sandboxed = false;      // the running process is inside murasaki-sandbox
    bool requireSandbox = true;  // the setting importRequireSandbox
    bool fallbackOnce = false;   // the sandbox helper itself failed (it could not run the player): the next start only runs without it, then confinement is tried again
};

ProcessPetAudioLink::ProcessPetAudioLink(QString assetsDir, QString program, QObject *parent) : PetAudioLink(parent), m(std::make_unique<Impl>())
{
    m->assets = std::move(assetsDir);
    m->program = std::move(program);
}

ProcessPetAudioLink::~ProcessPetAudioLink()
{
    stop();
}

bool ProcessPetAudioLink::running() const
{
    return m->process && m->process->state() != QProcess::NotRunning;
}

bool ProcessPetAudioLink::start()
{
    if (running())
        return true;
    QString program = m->program.isEmpty() ? findPetAudioHelper() : m->program;
    if (program.isEmpty())
        return false;
    if (m->process) { // (a QProcess is not destroyed inside its own finished signal: this may run from there)
        m->process->disconnect(this);
        m->process.release()->deleteLater();
    }
    m->process = std::make_unique<QProcess>();
    m->ready = false;
    m->queue.clear();
    m->pending.clear();
    QProcess *p = m->process.get();
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
        m->pending += p->readAllStandardOutput();
        if (m->pending.size() > 65536)
            m->pending.clear();
        qsizetype nl;
        while ((nl = m->pending.indexOf('\n')) >= 0) {
            const QString text = QString::fromUtf8(m->pending.left(nl)).trimmed();
            m->pending.remove(0, nl + 1);
            if (text == u"ready"_s) {
                m->ready = true;
                for (const QString &line : std::as_const(m->queue))
                    p->write((line + u'\n').toUtf8());
                m->queue.clear();
            } else if (!text.isEmpty()) {
                emit line(text);
            }
        }
    });
    connect(p, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        // Only the sandbox helper's own failures (usage, could not run the program: 125 to 127) can make a start unconfined, once, and never when confinement
        // is required. The player's status 3 (no audio system, normal at login before PipeWire is up) says nothing about the sandbox.
        if (m->sandboxed && !m->ready && !m->requireSandbox && (code == 125 || code == 126 || code == 127)) {
            m->fallbackOnce = true;
            qWarning("murasaki-overlay: the sandbox could not start the sound player (status %d); the next start runs without it, the one after tries again", code);
        }
        emit exited(code, status == QProcess::CrashExit);
    });
    connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit exited(127, false);
    });
    p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    // The player runs inside murasaki-sandbox when that is installed (docs/security.md): the system and the sounds folder to read, no TCP. This is the generic
    // policy, not the import profile: the player has to reach the PipeWire socket of the session, so it does not hide the runtime directory. With
    // importRequireSandbox (the default) it does not run at all without the sandbox. MURASAKI_PETAUDIO_SANDBOX=0 turns the sandbox off, only where it is not required.
    const QString helper = findSandboxHelper();
    const bool off = !m->requireSandbox && qEnvironmentVariable("MURASAKI_PETAUDIO_SANDBOX") == u"0"_s;
    if (m->requireSandbox && helper.isEmpty()) {
        qWarning("murasaki-overlay: the sound player needs murasaki-sandbox (importRequireSandbox) and it is not installed");
        m->process.reset();
        return false;
    }
    m->sandboxed = !helper.isEmpty() && !off && !m->fallbackOnce;
    m->fallbackOnce = false; // (one start only)
    if (m->sandboxed) {
        QStringList args;
        if (m->requireSandbox)
            args << u"--require"_s;
        for (const QString &ro : {u"/usr"_s, u"/lib"_s, u"/lib64"_s, u"/etc"_s, m->assets, program})
            if (QFileInfo::exists(ro))
                args << u"--ro"_s << ro;
        // (a test hook: the file the player logs its starts to may be written)
        if (const QString log = qEnvironmentVariable("MURASAKI_AUDIO_LOG"); !log.isEmpty() && QFileInfo::exists(log))
            args << u"--rw"_s << log;
        args << u"--no-net"_s << u"--"_s << program << u"--assets"_s << m->assets;
        p->start(helper, args);
    } else {
        p->start(program, {u"--assets"_s, m->assets});
    }
    return true;
}

void ProcessPetAudioLink::send(const QString &text)
{
    if (!running())
        return;
    if (m->ready)
        m->process->write((text + u'\n').toUtf8());
    else if (m->queue.size() < 256)
        m->queue << text;
}

void ProcessPetAudioLink::setRequireSandbox(bool require)
{
    m->requireSandbox = require;
}

void ProcessPetAudioLink::stop()
{
    if (!m->process)
        return;
    QProcess *p = m->process.release();
    p->disconnect(this);
    if (p->state() != QProcess::NotRunning) {
        p->write("quit\n");
        p->closeWriteChannel();
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
        QTimer::singleShot(1000, p, [p] { p->kill(); });
    } else {
        p->deleteLater();
    }
    m->ready = false;
}

// ---- the controller -----------------------------------------------------------------------------------------------

PetAudioController::PetAudioController(PetAudioLink *link, Clock clock, QObject *parent)
    : QObject(parent), m_link(link), m_clock(clock ? std::move(clock) : Clock(monotonicNs))
{
    connect(m_link, &PetAudioLink::line, this, &PetAudioController::onLine);
    connect(m_link, &PetAudioLink::exited, this, &PetAudioController::onExited);
}

PetAudioController::Level PetAudioController::levelOf(const Pet &pet)
{
    if (!pet.shown)
        return Level::Gone;
    return pet.paused ? Level::Paused : Level::Active;
}

int PetAudioController::freeSlot() const
{
    for (int i = 0; i < 256; ++i)
        if (!m_slots.contains(i))
            return i;
    return -1;
}

PetAudioController::State *PetAudioController::bySlot(int slot)
{
    const auto it = m_slots.constFind(slot);
    if (it == m_slots.cend())
        return nullptr;
    const auto pet = m_pets.find(*it);
    return pet == m_pets.end() ? nullptr : &pet->second;
}

void PetAudioController::sendLine(const QString &line)
{
    static const bool debug = qEnvironmentVariableIsSet("MURASAKI_DEBUG_AUDIO"); // logs what the player is told (docs/pets/audio.md)
    if (debug)
        fprintf(stderr, "murasaki-overlay: audio -> %s%s\n", qPrintable(line), m_link->running() ? "" : " (no player)");
    if (m_link->running())
        m_link->send(line);
}

int PetAudioController::effectiveGain(const State &s) const
{
    if (m_muteWhenFullscreen && (m_fullscreen.contains(s.pet.output) || m_fullscreen.contains(u"*"_s)))
        return 0;
    return s.pet.audio.volume * 10;
}

bool PetAudioController::ensurePlayer()
{
    if (m_failed) {
        // Three deaths in a minute: given up for half a minute, then tried again (a PipeWire that was restarting has come back by then).
        if (now() / 1'000'000 - m_failedAt < kUnavailableRetryMs)
            return false;
        m_failed = false;
        m_crashes.clear();
    }
    if (m_unavailable) {
        if (now() / 1'000'000 - m_unavailableAt < kUnavailableRetryMs)
            return false;
        m_unavailable = false;
    }
    if (m_link->running())
        return true;
    if (!m_link->start()) {
        m_unavailable = true;
        m_unavailableAt = now() / 1'000'000;
        return false;
    }
    applyMaster();
    // A player that was started again knows nothing: the pets that hold a long sound ask for it once more.
    for (auto &[id, s] : m_pets) {
        s.voice = false;
        s.voicePaused = false;
        s.playing = false;
    }
    return true;
}

void PetAudioController::applyMaster()
{
    sendLine(u"master %1"_s.arg(m_muted ? 0 : m_volume * 10));
}

void PetAudioController::applyGains()
{
    for (auto &[id, s] : m_pets) {
        const int gain = effectiveGain(s);
        if (s.voice && gain != s.sentGain) {
            sendLine(u"gain %1 %2"_s.arg(s.slot).arg(gain));
            s.sentGain = gain;
        }
    }
}

void PetAudioController::play(State &s, qint64 baseNs, bool loop, bool applyOffset, bool force, bool scheduled)
{
    // `force` is the test button: it plays a pet whose mode is none (the sound is chosen, not yet switched on).
    if (s.slot < 0 || s.pet.audio.asset.isEmpty() || (!force && !s.pet.audio.active()))
        return;
    const bool wasRunning = m_link->running();
    if (!ensurePlayer())
        return;
    if (!wasRunning) { // after a (re)start, the other pets that should be holding a sound take it again
        for (auto &[id, other] : m_pets)
            if (&other != &s && other.level == Level::Active && other.pet.audio.active() && other.pet.audio.mode == PetAudioMode::Continuous)
                play(other, now(), true);
    }
    qint64 at = baseNs;
    qint64 startFrame = 0;
    const int offset = applyOffset ? s.pet.audio.offsetMs : 0;
    if (offset > 0)
        at += qint64(offset) * 1'000'000;
    else if (offset < 0)
        startFrame = qint64(-offset) * kFramesPerMs;
    if (scheduled && at < now() - kLateNs)
        return; // the moment of the wrap has passed: playing it now would be out of time
    audio::Command c;
    c.kind = audio::Command::Kind::Play;
    c.pet = s.slot;
    c.generation = s.generation;
    c.asset = s.pet.audio.asset.toStdString();
    c.startFrame = startFrame;
    c.gain = effectiveGain(s);
    s.sentGain = c.gain;
    c.loop = loop;
    // A wrap has its own time; a start that is "now" has none (0), so that a player which took a while to come up still plays it.
    c.atNs = (scheduled || offset > 0) ? std::max<qint64>(1, at) : 0;
    s.voice = true;
    s.voicePaused = false;
    s.problem.clear();
    sendLine(QString::fromStdString(audio::formatCommand(c)));
    // A start that is now (the test button) discards the starts scheduled for wraps in the mixer: the wrap that is about to come is sent again.
    if (!scheduled && s.pet.audio.mode == PetAudioMode::AnimationLoop && s.pendingWrapNs > now())
        play(s, s.pendingWrapNs, false, true, false, true);
}

void PetAudioController::stopVoice(State &s)
{
    if (s.voice && s.slot >= 0)
        sendLine(u"stop %1"_s.arg(s.slot));
    s.voice = false;
    s.voicePaused = false;
    s.playing = false;
    s.pendingWrapNs = 0;
}

void PetAudioController::pauseVoice(State &s)
{
    if (s.voice && !s.voicePaused) {
        sendLine(u"pause %1"_s.arg(s.slot));
        s.voicePaused = true;
    }
}

void PetAudioController::resumeVoice(State &s)
{
    if (s.voice && s.voicePaused) {
        sendLine(u"resume %1"_s.arg(s.slot));
        s.voicePaused = false;
    }
}

void PetAudioController::enter(State &s, Level)
{
    const PetAudio &a = s.pet.audio;
    if (!a.active())
        return;
    switch (a.mode) {
    case PetAudioMode::Once:
        play(s, now(), false);
        break;
    case PetAudioMode::Continuous:
        if (s.voice && s.voicePaused)
            resumeVoice(s);
        else if (!s.voice)
            play(s, now(), true);
        break;
    default: // animation_loop waits for the wrap, click for the click
        break;
    }
}

void PetAudioController::leave(State &s, Level to)
{
    if (to == Level::Paused && s.pet.audio.mode == PetAudioMode::Continuous)
        pauseVoice(s);
    else
        stopVoice(s);
}

void PetAudioController::setPet(const QString &petId, const Pet &pet)
{
    auto it = m_pets.find(petId);
    if (it == m_pets.end()) {
        const int slot = freeSlot();
        if (slot < 0)
            return;
        it = m_pets.emplace(petId, State{}).first;
        it->second.slot = slot;
        it->second.generation = m_nextGeneration++;
        m_slots.insert(slot, petId);
    }
    State &s = it->second;
    const Pet old = s.pet;
    const Level oldLevel = s.level;
    s.pet = pet;
    s.level = levelOf(pet);
    const bool changed = old.audio.asset != pet.audio.asset || old.audio.mode != pet.audio.mode || old.audio.offsetMs != pet.audio.offsetMs;
    if (changed) {
        s.generation = m_nextGeneration++;
        s.problem.clear();
        if (s.voice && oldLevel != Level::Gone)
            sendLine(u"stop %1"_s.arg(s.slot));
        s.voice = false;
        s.voicePaused = false;
        s.playing = false;
        if (s.level == Level::Active) {
            enter(s, oldLevel);
            // The edit killed a start scheduled for a wrap about to come: schedule it again under the new settings.
            if (pet.audio.mode == PetAudioMode::AnimationLoop && s.lastWrapNs > now() - kLateNs / 2) {
                s.pendingWrapNs = s.lastWrapNs > now() ? s.lastWrapNs : 0;
                play(s, s.lastWrapNs, false, true, false, true);
            }
        }
        return;
    }
    if (s.level != oldLevel) {
        if (s.level == Level::Active)
            enter(s, oldLevel);
        else
            leave(s, s.level);
    }
    if (s.voice && (old.audio.volume != pet.audio.volume || old.output != pet.output)) {
        s.sentGain = effectiveGain(s);
        sendLine(u"gain %1 %2"_s.arg(s.slot).arg(s.sentGain));
    }
}

void PetAudioController::removePet(const QString &petId)
{
    const auto it = m_pets.find(petId);
    if (it == m_pets.end())
        return;
    stopVoice(it->second);
    m_slots.remove(it->second.slot);
    m_pets.erase(it);
}

void PetAudioController::setVolume(int volume)
{
    volume = std::clamp(volume, 0, 100);
    if (volume == m_volume)
        return;
    m_volume = volume;
    applyMaster();
}

void PetAudioController::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    applyMaster();
}

void PetAudioController::setMuteWhenFullscreen(bool on)
{
    if (on == m_muteWhenFullscreen)
        return;
    m_muteWhenFullscreen = on;
    applyGains();
}

void PetAudioController::setFullscreenOutputs(const QSet<QString> &outputs)
{
    if (outputs == m_fullscreen)
        return;
    m_fullscreen = outputs;
    applyGains();
}

void PetAudioController::onWrap(const QString &petId, qint64 monotonicNs, qint64 loop)
{
    static const bool debug = qEnvironmentVariableIsSet("MURASAKI_DEBUG_AUDIO");
    if (debug) {
        const auto found = m_pets.find(petId);
        fprintf(stderr, "murasaki-overlay: audio wrap of %s loop %lld in %.0f ms (%s, level %d, mode %s)\n", qPrintable(petId.left(8)), loop,
                double(monotonicNs - now()) / 1e6, found == m_pets.end() ? "unknown pet" : "known", found == m_pets.end() ? -1 : int(found->second.level),
                found == m_pets.end() ? "-" : petAudioModeName(found->second.pet.audio.mode));
    }
    const auto it = m_pets.find(petId);
    if (it == m_pets.end() || it->second.level != Level::Active || it->second.pet.audio.mode != PetAudioMode::AnimationLoop)
        return;
    State &s = it->second;
    s.lastWrapNs = monotonicNs;
    s.pendingWrapNs = monotonicNs > now() ? monotonicNs : 0;
    play(s, monotonicNs, false, true, false, true);
}

void PetAudioController::onWrapCancelled(const QString &petId)
{
    const auto it = m_pets.find(petId);
    if (it == m_pets.end())
        return;
    State &s = it->second;
    if (s.pendingWrapNs > now() && s.slot >= 0)
        sendLine(u"cancel %1"_s.arg(s.slot));
    s.pendingWrapNs = 0;
    s.lastWrapNs = 0;
}

void PetAudioController::onClick(const QString &petId)
{
    const auto it = m_pets.find(petId);
    if (it == m_pets.end())
        return;
    State &s = it->second;
    // A click-through pet gets no pointer at all; this is the second check, for a source that sees the cursor over it.
    if (s.level != Level::Active || s.pet.audio.mode != PetAudioMode::Click || s.pet.clickThrough)
        return;
    play(s, now(), false);
}

bool PetAudioController::playNow(const QString &petId)
{
    const auto it = m_pets.find(petId);
    if (it == m_pets.end() || it->second.pet.audio.asset.isEmpty())
        return false;
    play(it->second, now(), false, false, true);
    return true;
}

void PetAudioController::stopAll()
{
    for (auto &[id, s] : m_pets)
        stopVoice(s);
}

void PetAudioController::stopOneShots()
{
    for (auto &[id, s] : m_pets)
        if (s.pet.audio.mode != PetAudioMode::Continuous)
            stopVoice(s);
}

void PetAudioController::resetFailures()
{
    m_failed = false;
    m_unavailable = false;
    m_crashes.clear();
}

void PetAudioController::onLine(const QString &text)
{
    const QStringList w = text.split(u' ');
    if (w.size() < 3)
        return;
    bool okSlot = false, okGen = false;
    const int slot = w[1].toInt(&okSlot);
    const unsigned gen = w[2].toUInt(&okGen);
    if (!okSlot || !okGen)
        return;
    State *s = bySlot(slot);
    if (!s || s->generation != gen)
        return;
    if (w[0] == u"started"_s) {
        s->playing = true;
    } else if (w[0] == u"ended"_s) {
        s->playing = false;
        s->voice = false;
    } else if (w[0] == u"dropped"_s) {
        s->playing = false;
        s->voice = false;
    } else if (w[0] == u"failed"_s) {
        s->playing = false;
        s->voice = false;
        s->problem = w.mid(3).join(u' ');
    }
}

void PetAudioController::onExited(int code, bool crashed)
{
    for (auto &[id, s] : m_pets) {
        s.voice = false;
        s.voicePaused = false;
        s.playing = false;
    }
    if (code == 3 || code == 125 || code == 126 || code == 127) { // no audio system yet (normal at login), or the sandbox could not run it: not a crash
        m_unavailable = true;
        m_unavailableAt = now() / 1'000'000;
        return;
    }
    if (code == 0 && !crashed)
        return; // it left on its own: idle, or the pipe was closed
    const qint64 t = now() / 1'000'000;
    m_crashes.append(t);
    while (!m_crashes.isEmpty() && t - m_crashes.first() > kCrashWindowMs)
        m_crashes.removeFirst();
    if (m_crashes.size() >= kMaxCrashes) {
        m_failed = true;
        m_failedAt = t;
        return;
    }
    // A player that ended without being asked (PipeWire restarted, a crash) takes its sounds with it: the pets that hold a long one ask again, which
    // starts a new player. The others wait for their next wrap, click or visit.
    for (auto &[id, s] : m_pets)
        if (s.level == Level::Active && !s.voice && s.pet.audio.active() && s.pet.audio.mode == PetAudioMode::Continuous)
            play(s, now(), true); // (not one the first play of this loop already took again)
}

QJsonObject PetAudioController::status(const QString &petId) const
{
    const auto it = m_pets.find(petId);
    if (it == m_pets.end())
        return {};
    const State &s = it->second;
    const PetAudio &a = s.pet.audio;
    QString state = u"off"_s;
    QStringList degraded;
    if (a.active()) {
        if (m_failed)
            state = u"failed"_s;
        else if (m_unavailable)
            state = u"unavailable"_s;
        else
            state = s.playing ? u"playing"_s : u"idle"_s;
        if (m_failed)
            degraded << name(kAudioFailed);
        if (m_unavailable)
            degraded << name(kAudioUnavailable);
        if (!s.problem.isEmpty())
            degraded << s.problem;
        if (a.mode == PetAudioMode::AnimationLoop && s.pet.frames < 2)
            degraded << name(kAudioNoLoop);
        if (a.mode == PetAudioMode::Click && s.pet.clickThrough)
            degraded << name(kAudioClickNeedsClickable);
    }
    return QJsonObject{{u"state"_s, state}, {u"mode"_s, QString::fromLatin1(petAudioModeName(a.mode))}, {u"degraded"_s, QJsonArray::fromStringList(degraded)}};
}

} // namespace murasaki
