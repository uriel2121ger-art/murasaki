#pragma once

#include "bridgeitem.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>

namespace murasaki {

// The picture of a puppet pet (docs/pets/puppets.md section 22): a BridgeItem whose host is `murasaki-puppet --engine` behind `murasaki-sandbox --profile puppet`. The base class launches it with an inherited socket, imports the frames
// it announces (the picture has real alpha, premultiplied) and restarts it with a backoff; this adds what is the puppet's own: the command line (PuppetEngineHost::command, the one the host tests pin), the
// telling of a refusal from a crash (puppet/exitcodes.h, shared with PuppetEngineHost), the footprint the engine sends (HitMask) and the commands that go the other way (play a motion group, set an expression, look at the pointer).
//
// The policy is the overlay controller's, not this item's: a crash is reported (`engineCrashed`, with whether the engine had been drawing for kEngineLongRunSeconds), the controller records it in the CrashLedger and, at the third, sets `blocked`,
// which stops the restarts. A refusal (the model, no GPU, no sandbox) ends the restarts here and is reported once (`engineRefused`, a name of puppetnames.h or a short reason).
class PuppetItem : public BridgeItem
{
    Q_OBJECT
    Q_PROPERTY(QString itemDir READ itemDir WRITE setItemDir NOTIFY itemDirChanged)
    Q_PROPERTY(QString modelFile READ modelFile WRITE setModelFile NOTIFY modelFileChanged)
    Q_PROPERTY(bool sandbox READ sandbox WRITE setSandbox NOTIFY sandboxChanged)
    Q_PROPERTY(bool requireSandbox READ requireSandbox WRITE setRequireSandbox NOTIFY requireSandboxChanged)
    Q_PROPERTY(qreal maxPixels READ maxPixels WRITE setMaxPixels NOTIFY maxPixelsChanged)
    Q_PROPERTY(bool blocked READ blocked WRITE setBlocked NOTIFY blockedChanged)
    Q_PROPERTY(bool look READ look WRITE setLook NOTIFY lookChanged)
    Q_PROPERTY(QString expression READ expression WRITE setExpression NOTIFY expressionChanged)

public:
    explicit PuppetItem(QQuickItem *parent = nullptr);

    QString itemDir() const { return m_itemDir; }
    void setItemDir(const QString &dir);
    // The model file, relative to the item folder (the engine opens it with openat2 from there).
    QString modelFile() const { return m_modelFile; }
    void setModelFile(const QString &file);
    bool sandbox() const { return m_sandbox; }
    void setSandbox(bool on);
    bool requireSandbox() const { return m_requireSandbox; }
    void setRequireSandbox(bool on);
    // The most pixels the picture may have (the setting puppetMaxPixels): a bigger pet is drawn smaller by the engine and scaled up here.
    qreal maxPixels() const { return m_maxPixels; }
    void setMaxPixels(qreal pixels);
    // Whether the picture is drawn below the pet's size because of maxPixels.
    bool resolutionReduced() const { return m_reduced; }
    // The model took the engine down three times (the controller's ledger says so): no more restarts, and the host that runs is stopped.
    bool blocked() const { return m_blocked; }
    void setBlocked(bool blocked);
    // Looking at the pointer (the gaze and head parameters), and the expression that shows; both are state, re-sent when the engine restarts.
    bool look() const { return m_look; }
    void setLook(bool on);
    QString expression() const { return m_expression; }
    void setExpression(const QString &name);

    // Plays a motion group of the model (priority "idle", "normal" or "force"). The last request is kept until stopMotion(): it is sent when the engine is ready, and again after a restart.
    Q_INVOKABLE void playMotion(const QString &group, const QString &priority = QStringLiteral("normal"));
    // A click on the picture at (u, v), fractions of it: the engine plays the motion group of the hit area it is on, else the first of `fallback` the model has.
    Q_INVOKABLE void clickAt(double u, double v, const QStringList &fallback);
    // Ends the motion that plays (the model goes back to its own life).
    Q_INVOKABLE void stopMotion();
    // The pointer at (x, y) device pixels of the picture, for the gaze. Only meaningful while `look` is on.
    Q_INVOKABLE void pointerAt(double x, double y);
    // The command line of a launch (the program first), for the tests.
    static QStringList launchCommand(const QString &enginePath, const QString &sandboxPath, bool requireSandbox, const QString &itemDir, const QString &file, const QSize &size, int fd, int fps);

signals:
    void itemDirChanged();
    void modelFileChanged();
    void sandboxChanged();
    void requireSandboxChanged();
    void blockedChanged();
    void maxPixelsChanged();
    void lookChanged();
    void expressionChanged();
    // The engine died of something that is not a refusal. `afterLongRun`: it had been drawing for kEngineLongRunSeconds (the ledger then starts its count again).
    void engineCrashed(bool afterLongRun);
    // The engine will not be started again: "model refused", "puppet: no GPU" or "sandbox unavailable".
    void engineRefused(const QString &name);
    // The footprint of the picture (bridge/puppetview.h): cols x rows bits of `cell` pixel cells, row 0 first, bit 0 of a byte first.
    void hitMask(int cols, int rows, int cell, const QByteArray &bits);
    // The first picture of each run of the engine (the first start and every restart: `hasFrame` stays true across a restart, so it says nothing about the new run).
    void pictureShown();

protected:
    bool canStart() const override;
    BridgeLaunch buildLaunch(int bridgeFd, const QSize &size) const override;
    QSize bufferSize(const QSize &pixels) const override;
    void hostExited(int code, QProcess::ExitStatus status) override;
    void retiredHostExited(int code, QProcess::ExitStatus status) override;
    void clientReady(BridgeClient *client) override;
    QString logName(const BridgeLaunch &launch) const override;

private:
    void sendState();
    void sendObject(const QJsonObject &object);

    QString m_itemDir;
    QString m_modelFile;
    bool m_sandbox = true;
    bool m_requireSandbox = true;
    qreal m_maxPixels = 4194304;
    mutable bool m_reduced = false;
    bool m_blocked = false;
    bool m_refused = false; // an end that is not retried
    bool m_look = false;
    QString m_expression;
    QString m_motion;         // the group last asked for ("" none) and its priority
    QString m_motionPriority;
    mutable bool m_runFramed = false; // this run has shown a picture
    mutable QElapsedTimer m_drawing; // since the first picture of this run (reset by the const buildLaunch)
};

} // namespace murasaki
