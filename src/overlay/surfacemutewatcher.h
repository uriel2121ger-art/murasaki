#pragma once

#include "dbusbackend.h"

#include <QHash>
#include <QObject>
#include <QSet>

namespace murasaki {

class PetAudioController;

// Follows the policy server and tells the sound controller where the pets should be quiet. From revision 16 the surface says it per output (SessionFacts():
// the outputs with a fullscreen window, and `quiet` for a game or a listed application, which silences every output); an older surface only has PetAudioMuted(),
// a single answer for all outputs. Without a policy server the rule is off. The backend is the Surface object of the session bus; tests give a fake.
class SurfaceMuteWatcher : public QObject
{
    Q_OBJECT

public:
    SurfaceMuteWatcher(DBusBackend *surface, PetAudioController *audio, QObject *parent = nullptr);
    void start();
    bool muted() const { return m_muted; }
    // The outputs that are quiet now ("*" is all of them).
    QSet<QString> mutedOutputs() const { return m_outputs; }

signals:
    // Which outputs have a fullscreen window (the application's class, "" when not told; "*" is every output: a game or a listed application), and whether the
    // surface can say at all. The fullscreen mode of the pets (docs/pets/stage.md) hides on it, and the sound controller is given the same set.
    void fullscreenChanged(const QHash<QString, QString> &apps, bool known);
    // The screens a game plays on, by the wallpaper's rule for pauseOnGame: while a game runs, the outputs a window covers (fullscreen or occluded), or every
    // output ("*") when the session cannot say which windows cover what. Empty without a game, and from a surface older than revision 16 (it cannot tell).
    void gameScreensChanged(const QSet<QString> &outputs);

private:
    void set(const QSet<QString> &outputs, const QHash<QString, QString> &apps = {}, bool known = false);
    void read();
    void readOld(quint64 seq);
    void applyFacts(const QString &json);
    void setGameScreens(const QSet<QString> &outputs);

    DBusBackend *m_surface;
    PetAudioController *m_audio;
    bool m_muted = false;
    bool m_perOutput = false; // the surface has SessionFacts: PetAudioMutedChanged is then ignored
    QSet<QString> m_outputs;
    QSet<QString> m_gameScreens;
    quint64 m_seq = 0;
};

} // namespace murasaki
