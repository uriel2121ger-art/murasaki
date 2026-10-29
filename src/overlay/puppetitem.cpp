#include "puppetitem.h"

#include "bridgeclient.h"
#include "puppetenginehost.h"
#include "puppetlimits.h"
#include "puppetnames.h"
#include "sceneenginelookup.h"

#include "puppet/exitcodes.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;
namespace puppet = murasaki::puppet;

namespace murasaki {

PuppetItem::PuppetItem(QQuickItem *parent)
    : BridgeItem(parent)
{
    setHostName(u"puppet engine"_s);
    setFailureNoun(u"puppet"_s);
    setExitWaitMs(300); // a crashed engine closes its socket first, its status is what makes it a crash, and only one engine lives at a time (BridgeItem::setExitWaitMs)
}

void PuppetItem::setItemDir(const QString &dir)
{
    if (dir == m_itemDir)
        return;
    m_itemDir = dir;
    m_refused = false;
    emit itemDirChanged();
    if (isCompleted())
        restartHost();
}

void PuppetItem::setModelFile(const QString &file)
{
    if (file == m_modelFile)
        return;
    m_modelFile = file;
    m_refused = false;
    emit modelFileChanged();
    if (isCompleted())
        restartHost();
}

void PuppetItem::setSandbox(bool on)
{
    if (on == m_sandbox)
        return;
    m_sandbox = on;
    emit sandboxChanged();
    if (isCompleted())
        restartHost();
}

void PuppetItem::setRequireSandbox(bool on)
{
    if (on == m_requireSandbox)
        return;
    m_requireSandbox = on;
    emit requireSandboxChanged();
    if (isCompleted())
        restartHost();
}

void PuppetItem::setMaxPixels(qreal pixels)
{
    if (!(pixels >= 1) || pixels == m_maxPixels)
        return;
    m_maxPixels = pixels;
    emit maxPixelsChanged();
    if (isCompleted())
        restartHost(); // (a rare change: the engine is started again at the new size)
}

QSize PuppetItem::bufferSize(const QSize &pixels) const
{
    const double area = double(pixels.width()) * pixels.height();
    m_reduced = area > m_maxPixels && area > 0;
    if (!m_reduced)
        return pixels;
    const double scale = std::sqrt(m_maxPixels / area);
    return QSize(std::max(1, int(pixels.width() * scale)), std::max(1, int(pixels.height() * scale)));
}

void PuppetItem::setBlocked(bool blocked)
{
    if (blocked == m_blocked)
        return;
    m_blocked = blocked;
    emit blockedChanged();
    if (isCompleted())
        restartHost(); // (stops the host when blocked: canStart() says no; starts it again when unblocked)
}

void PuppetItem::setLook(bool on)
{
    if (on == m_look)
        return;
    m_look = on;
    emit lookChanged();
    sendObject(QJsonObject{{u"look"_s, on}});
}

void PuppetItem::setExpression(const QString &name)
{
    if (name == m_expression)
        return;
    m_expression = name;
    emit expressionChanged();
    sendObject(QJsonObject{{u"expression"_s, name.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(name)}});
}

void PuppetItem::playMotion(const QString &group, const QString &priority)
{
    m_motion = group;
    m_motionPriority = priority;
    sendObject(QJsonObject{{u"play"_s, QJsonObject{{u"group"_s, group}, {u"priority"_s, priority}}}});
}

void PuppetItem::clickAt(double u, double v, const QStringList &fallback)
{
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1))
        return;
    sendObject(QJsonObject{{u"click"_s, QJsonObject{{u"u"_s, u}, {u"v"_s, v}, {u"fallback"_s, QJsonArray::fromStringList(fallback.mid(0, 4))}}}});
}

void PuppetItem::stopMotion()
{
    m_motion.clear();
    sendObject(QJsonObject{{u"stop"_s, true}});
}

void PuppetItem::pointerAt(double x, double y)
{
    if (BridgeClient *client = hostClient(); client && client->isReady() && m_look)
        client->sendPointer(int(qRound(x)), int(qRound(y)), 0);
}

void PuppetItem::sendObject(const QJsonObject &object)
{
    if (BridgeClient *client = hostClient(); client && client->isReady())
        client->sendProperties(object);
}

void PuppetItem::sendState()
{
    // What the engine does not remember across a restart: the expression and whether it looks at the pointer.
    QJsonObject state{{u"look"_s, m_look}};
    if (!m_expression.isEmpty())
        state.insert(u"expression"_s, m_expression);
    if (!m_motion.isEmpty())
        state.insert(u"play"_s, QJsonObject{{u"group"_s, m_motion}, {u"priority"_s, m_motionPriority}});
    sendObject(state);
}

void PuppetItem::clientReady(BridgeClient *client)
{
    connect(client, &BridgeClient::frameAvailable, this, [this] {
        if (m_runFramed)
            return;
        m_runFramed = true;
        m_drawing.start();
        emit pictureShown();
    });
    connect(client, &BridgeClient::hitMaskReceived, this, [this](uint32_t cols, uint32_t rows, uint32_t cell, const QByteArray &bits) { emit hitMask(int(cols), int(rows), int(cell), bits); });
    sendState();
}

bool PuppetItem::canStart() const
{
    return !m_itemDir.isEmpty() && !m_modelFile.isEmpty() && !m_blocked && !m_refused;
}

QStringList PuppetItem::launchCommand(const QString &enginePath, const QString &sandboxPath, bool requireSandbox, const QString &itemDir, const QString &file, const QSize &size, int fd, int fps)
{
    PuppetEngineHost::Config config;
    config.enginePath = enginePath;
    config.sandboxPath = sandboxPath;
    config.requireSandbox = requireSandbox;
    config.fps = fps;
    return PuppetEngineHost::command(config, itemDir, file, size, fd);
}

BridgeLaunch PuppetItem::buildLaunch(int bridgeFd, const QSize &size) const
{
    BridgeLaunch result;
    m_drawing.invalidate();
    m_runFramed = false;
    const QString engine = findPuppetHelper();
    if (engine.isEmpty()) {
        result.error = u"murasaki-puppet is not installed"_s;
        return result;
    }
    QString sandbox;
    if (m_sandbox || m_requireSandbox)
        sandbox = findSandboxHelper();
    if (sandbox.isEmpty() && m_requireSandbox) {
        result.error = u"the sandbox is required, but murasaki-sandbox is not installed"_s;
        return result;
    }
    if (sandbox.isEmpty())
        qWarning("murasaki: murasaki-sandbox not found; the puppet engine runs without a sandbox");
    QStringList command = launchCommand(engine, sandbox, m_requireSandbox, m_itemDir, m_modelFile, bufferSize(size), bridgeFd, fps());
    result.program = command.takeFirst();
    result.arguments = command;
    const QSize drawn = bufferSize(size);
    result.size = drawn;
    result.carriesState = false; // the fps goes on the command line too, and the rest is sent live after Hello
    return result;
}

void PuppetItem::hostExited(int code, QProcess::ExitStatus status)
{
    const bool afterLongRun = m_drawing.isValid() && m_drawing.elapsed() >= puppet::kEngineLongRunSeconds * 1000;
    m_drawing.invalidate();
    switch (puppet::classifyExit(status != QProcess::NormalExit, code)) {
    case puppet::ExitClass::Clean:
        break;
    case puppet::ExitClass::ModelRefused:
        m_refused = true;
        emit engineRefused(u"model refused"_s);
        break;
    case puppet::ExitClass::NoGpu:
        m_refused = true;
        emit engineRefused(QString::fromUtf8(puppet::kNoGpu.data(), qsizetype(puppet::kNoGpu.size())));
        break;
    case puppet::ExitClass::SandboxUnavailable:
        m_refused = true;
        emit engineRefused(u"sandbox unavailable"_s);
        break;
    case puppet::ExitClass::Crash:
        emit engineCrashed(afterLongRun);
        break;
    }
}

void PuppetItem::retiredHostExited(int code, QProcess::ExitStatus status)
{
    // An engine that falls over while it is being stopped still counts against the model (a refusal does not: the next engine says it again).
    if (puppet::classifyExit(status != QProcess::NormalExit, code) == puppet::ExitClass::Crash)
        emit engineCrashed(false);
}

QString PuppetItem::logName(const BridgeLaunch &) const
{
    return u"murasaki-puppet"_s;
}

} // namespace murasaki
