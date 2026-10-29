#pragma once

// When the pets make their sounds (docs/pets/audio.md). The overlay's controller tells this class what each pet is (its sound settings, whether it
// is shown, paused, click-through) and what happens to it (the animation wrapped, a click); this class decides what the player process is told. It
// plays nothing itself: the player (murasaki-petaudio) is a separate process, started on the first sound, reached through a PetAudioLink.

#include "constants.h"
#include "petaudio.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>

namespace murasaki {

// The line to the player process. An interface so the tests script it.
class PetAudioLink : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    virtual bool start() = 0;               // false: the player can not be started at all
    virtual bool running() const = 0;
    virtual void send(const QString &line) = 0;
    virtual void stop() = 0;
    // Whether the player must run inside murasaki-sandbox (the setting importRequireSandbox): then it does not run at all without it.
    virtual void setRequireSandbox(bool) {}

signals:
    void line(const QString &text);         // one answer of the player
    void exited(int code, bool crashed);    // code 3: the player started but has no audio system
};

// The real link: murasaki-petaudio as a child process, its stdin and stdout.
class ProcessPetAudioLink final : public PetAudioLink
{
    Q_OBJECT

public:
    // `assetsDir` is where the assets are (the player is told); `program` empty: found beside the overlay, then in PATH.
    ProcessPetAudioLink(QString assetsDir, QString program = {}, QObject *parent = nullptr);
    ~ProcessPetAudioLink() override;
    bool start() override;
    bool running() const override;
    void send(const QString &line) override;
    void stop() override;
    void setRequireSandbox(bool require) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

class PetAudioController : public QObject
{
    Q_OBJECT

public:
    // The monotonic clock the player's start times are on (CLOCK_MONOTONIC), in nanoseconds; tests give their own.
    using Clock = std::function<qint64()>;
    static constexpr int kMaxCrashes = 3;
    static constexpr qint64 kCrashWindowMs = 60'000;

    // What the controller knows of a pet.
    struct Pet {
        PetAudio audio;
        bool clickThrough = true;
        bool shown = false;       // it has a window on an output (and the pets are not hidden)
        bool paused = false;      // the placement is paused, or the session is locked / the screens are off
        QString output;
        int frames = 0;           // of the animation it plays: fewer than two never wraps
    };

    explicit PetAudioController(PetAudioLink *link, Clock clock = {}, QObject *parent = nullptr);

    // ---- what the pets are
    void setPet(const QString &petId, const Pet &pet);
    void removePet(const QString &petId);
    // ---- the global state (the daemon's settings and the surface)
    void setVolume(int volume);                         // 0..100, the master
    void setMuted(bool muted);
    void setMuteWhenFullscreen(bool on);
    // The setting importRequireSandbox, which the player follows as the imports do (default: required).
    void setRequireSandbox(bool require) { m_link->setRequireSandbox(require); }
    void setFullscreenOutputs(const QSet<QString> &outputs); // the outputs a fullscreen window or a listed app is up on; "*" is all of them
    // ---- what happens
    // The animation wraps (or wrapped) at `monotonicNs`, announced a little ahead: the sound is scheduled for that exact time. A cancelled announcement
    // drops the scheduled start (not a sound already playing).
    void onWrap(const QString &petId, qint64 monotonicNs, qint64 loop);
    void onWrapCancelled(const QString &petId);
    void onClick(const QString &petId);
    // ---- the test button
    bool playNow(const QString &petId);
    void stopAll();
    // Stops what plays once (a click, a wrap, a start): a pet in the mode continuous keeps its sound (the automation's "silence the pets"; use mute to silence all).
    void stopOneShots();
    // Forgets that the player failed too often and allows it again at once (otherwise it is tried again after 30 s).
    void resetFailures();

    // {"state": "off"|"idle"|"playing"|"unavailable"|"failed", "mode", "degraded": [names]} for a pet; an empty object for an unknown one.
    QJsonObject status(const QString &petId) const;
    bool playerRunning() const { return m_link->running(); }

private:
    enum class Level { Gone, Paused, Active };
    struct State {
        Pet pet;
        int slot = -1;
        unsigned generation = 0;   // of the sound settings: a command of an older one is not acted on
        Level level = Level::Gone;
        bool voice = false;        // a sound was started for it and has not been seen to end
        bool voicePaused = false;
        qint64 lastWrapNs = 0;     // the latest announced wrap
        qint64 pendingWrapNs = 0;  // a start scheduled for a wrap that has not come yet
        int sentGain = -1;         // the gain (thousandths) the player was last given for the voice
        bool playing = false;      // the player said it started
        QString problem;           // a name of petaudiolimits.h the player reported (asset missing, invalid)
    };

    static Level levelOf(const Pet &pet);
    qint64 now() const { return m_clock(); }
    bool ensurePlayer();
    void sendLine(const QString &line);
    int effectiveGain(const State &s) const; // thousandths
    // `scheduled`: baseNs is a moment of the animation's (a wrap), so a start too long past is dropped by the player; otherwise baseNs is "now" and the
    // start has no time of its own (0: at once) unless the offset delays it.
    void play(State &s, qint64 baseNs, bool loop, bool applyOffset = true, bool force = false, bool scheduled = false);
    void stopVoice(State &s);
    void pauseVoice(State &s);
    void resumeVoice(State &s);
    void enter(State &s, Level from);       // becoming Active
    void leave(State &s, Level to);         // becoming Paused or Gone
    void applyMaster();
    void applyGains();
    void onLine(const QString &text);
    void onExited(int code, bool crashed);
    State *bySlot(int slot);
    int freeSlot() const;

    PetAudioLink *m_link;
    Clock m_clock;
    std::map<QString, State> m_pets;
    QHash<int, QString> m_slots;
    unsigned m_nextGeneration = 1;
    int m_volume = kDefaultPetsVolume;
    bool m_muted = false;
    bool m_muteWhenFullscreen = true;
    QSet<QString> m_fullscreen;
    bool m_unavailable = false;   // the player started and said there is no audio system
    qint64 m_unavailableAt = 0;
    bool m_failed = false;        // it crashed too often: given up on for 30 s
    qint64 m_failedAt = 0;        // ms
    QList<qint64> m_crashes;      // ms
};

} // namespace murasaki
