#pragma once

#include <QList>
#include <QObject>
#include <QTimer>

#include <memory>

namespace murasaki {

struct AudioCaptureImpl;

// Captures what the default output device plays (PipeWire monitor stream) and publishes its 128-value spectrum
// about every 33 ms. Lives on the GUI thread. Without PipeWire support compiled in it only ever reports silence.
class AudioCapture : public QObject
{
    Q_OBJECT

public:
    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture() override;

    bool isActive() const { return m_wanted; }
    // Idle (false) tears the stream down completely, so no capture indicator stays on and no CPU is used.
    // While active, a failed or lost stream is retried with a growing delay.
    void setActive(bool active);

signals:
    void spectrumChanged(const QList<float> &spectrum);

private:
    bool start();
    void stop();
    void publish();
    void emitSilence();
    void handleStreamFailure(quint64 generation);

    bool attempt();
    void scheduleRetry();

    std::unique_ptr<AudioCaptureImpl> m_impl;
    QTimer m_timer;
    QTimer m_retryTimer;
    int m_backoffMs;
    bool m_wanted = false; // what the caller asked for, independent of the stream's health
    bool m_active = false; // a stream is currently set up
};

} // namespace murasaki
