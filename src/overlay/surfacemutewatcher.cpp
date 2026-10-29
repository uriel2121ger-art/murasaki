#include "surfacemutewatcher.h"

#include "petaudiocontroller.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace murasaki {

using namespace Qt::StringLiterals;

SurfaceMuteWatcher::SurfaceMuteWatcher(DBusBackend *surface, PetAudioController *audio, QObject *parent)
    : QObject(parent), m_surface(surface), m_audio(audio)
{
    connect(m_surface, &DBusBackend::availabilityChanged, this, [this](bool available) {
        if (available) {
            read();
        } else {
            m_perOutput = false;
            set({}); // no policy server: nobody to say a window is fullscreen
            setGameScreens({}); // (nor that a game plays)
        }
    });
    connect(m_surface, &DBusBackend::signalReceived, this, [this](const QString &name, const QVariantList &args) {
        if (name == u"SessionFactsChanged"_s) {
            read();
        } else if (name == u"PetAudioMutedChanged"_s && !args.isEmpty() && !m_perOutput) {
            ++m_seq; // a read in flight is older than this
            set(args.at(0).toBool() ? QSet<QString>{u"*"_s} : QSet<QString>{});
        }
    });
}

void SurfaceMuteWatcher::start()
{
    m_surface->start();
}

void SurfaceMuteWatcher::read()
{
    const quint64 seq = ++m_seq;
    m_surface->call(u"SessionFacts"_s, {}, [this, seq](const DBusBackend::Reply &reply) {
        if (seq != m_seq)
            return;
        if (reply.ok) {
            m_perOutput = true;
            applyFacts(reply.value.toString());
        } else {
            m_perOutput = false; // (a surface that stopped answering SessionFacts is read the old way again)
            setGameScreens({});  // (and that way says nothing of the game's screens)
            readOld(seq);        // a surface of revision 15 has only the single answer
        }
    });
}

void SurfaceMuteWatcher::readOld(quint64 seq)
{
    m_surface->call(u"PetAudioMuted"_s, {}, [this, seq](const DBusBackend::Reply &reply) {
        if (seq == m_seq && reply.ok) // an older surface has no such call either: the rule stays off
            set(reply.value.toBool() ? QSet<QString>{u"*"_s} : QSet<QString>{});
    });
}

// `quiet` (a game or a listed application) silences every output; otherwise the outputs that have a fullscreen window.
void SurfaceMuteWatcher::applyFacts(const QString &json)
{
    const QJsonObject facts = QJsonDocument::fromJson(json.toUtf8()).object();
    QSet<QString> outputs;
    QHash<QString, QString> apps;
    if (facts.value(u"quiet"_s).toBool()) {
        outputs.insert(u"*"_s);
        apps.insert(u"*"_s, QString());
    } else {
        const QJsonObject all = facts.value(u"outputs"_s).toObject();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const QJsonObject output = it.value().toObject();
            if (output.value(u"fullscreen"_s).toBool()) {
                outputs.insert(it.key());
                apps.insert(it.key(), output.value(u"app"_s).toString()); // (only a Murasaki process is told the application)
            }
        }
    }
    set(outputs, apps, facts.value(u"known"_s).toBool(false));

    QSet<QString> gameScreens;
    if (facts.value(u"gameRunning"_s).toBool()) {
        if (!facts.value(u"known"_s).toBool(false)) {
            gameScreens.insert(u"*"_s);
        } else {
            const QJsonObject all = facts.value(u"outputs"_s).toObject();
            for (auto it = all.begin(); it != all.end(); ++it) {
                const QJsonObject output = it.value().toObject();
                if (output.value(u"fullscreen"_s).toBool() || output.value(u"occluded"_s).toBool())
                    gameScreens.insert(it.key());
            }
        }
    }
    setGameScreens(gameScreens);
}

void SurfaceMuteWatcher::setGameScreens(const QSet<QString> &outputs)
{
    if (outputs == m_gameScreens)
        return;
    m_gameScreens = outputs;
    emit gameScreensChanged(outputs);
}

void SurfaceMuteWatcher::set(const QSet<QString> &outputs, const QHash<QString, QString> &apps, bool known)
{
    m_outputs = outputs;
    m_muted = !outputs.isEmpty();
    m_audio->setFullscreenOutputs(outputs);
    // An older surface says one thing for all outputs and nothing of which; `known` is then whether anything was said.
    QHash<QString, QString> told = apps;
    if (apps.isEmpty() && !outputs.isEmpty()) {
        for (const QString &name : outputs)
            told.insert(name, QString());
    }
    emit fullscreenChanged(told, known || !outputs.isEmpty());
}

} // namespace murasaki
