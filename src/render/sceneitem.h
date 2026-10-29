#pragma once

#include "bridgeitem.h"
#include "constants.h"
#include "sceneengine.h"
#include "shortcut/shortcutitem.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

namespace murasaki {

// Shows a Wallpaper Engine scene rendered by the scene engine process (murasaki-scene-vk, over the Murasaki
// bridge). A thin BridgeItem: it looks the engine up, builds its command line from the scene settings and
// leaves the bridge, rendering, backoff and failure handling to the base class. `failed` is also emitted when
// the engine is not installed.
//
// Audio is off until `audible` is true (BridgeItem). The engine is deliberately launched with `--volume 0` and
// never `--silent`: a silent engine never loads a scene's sounds, and nothing could turn that on later. A volume of 0
// loads everything and only scales the mix, which the bridge's Volume message updates. The audio-reactive spectrum
// does not depend on either flag, so reactive scenes keep their input with both options.
//
// The engine is chosen at every launch (chooseSceneEngine): `sceneEngine` is the daemon's setting, `engineLabel` says
// what was chosen and why ("Vulkan", "Vulkan (incomplete: effects: M2)"). The Vulkan engine gets one restart after a
// crash on an item, see SceneCheckCache.
//
// With `sandbox` (the default) the engine runs behind murasaki-sandbox: a Landlock policy that lets it read the item,
// the assets and the system, write the GPU nodes and shader caches, and nothing else (docs/security.md). Without the
// helper installed, or on a kernel without Landlock, the engine runs as it always did and says so once in its log. With
// `requireSandbox` it does not run at all then: `failed` says why, and the scene is not shown.
//
// Changing itemDir, assetsDir, outputName, sceneEngine, sandbox or renderScale restarts the engine.
//
// An engine that stays paused for kParkAfterMs (a game or a window covering the output) is stopped to give its memory
// back, and started again when the pause ends; the last frame stays on screen meanwhile (BridgeItem::setParkAfterMs).
class SceneItem : public BridgeItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString itemDir READ itemDir WRITE setItemDir NOTIFY itemDirChanged)
    Q_PROPERTY(QString assetsDir READ assetsDir WRITE setAssetsDir NOTIFY assetsDirChanged)
    Q_PROPERTY(QString outputName READ outputName WRITE setOutputName NOTIFY outputNameChanged)
    Q_PROPERTY(QString desktop READ desktop WRITE setDesktop NOTIFY desktopChanged)
    Q_PROPERTY(QString sceneEngine READ sceneEngine WRITE setSceneEngine NOTIFY sceneEngineChanged)
    Q_PROPERTY(QString engineLabel READ engineLabel NOTIFY engineLabelChanged)
    Q_PROPERTY(bool sandbox READ sandbox WRITE setSandbox NOTIFY sandboxChanged)
    Q_PROPERTY(bool requireSandbox READ requireSandbox WRITE setRequireSandbox NOTIFY requireSandboxChanged)
    Q_PROPERTY(double renderScale READ renderScale WRITE setRenderScale NOTIFY renderScaleChanged)

public:
    // How long an unchecked item waits for `--check` before it starts (the verdict is usually back within 0.3 s).
    static constexpr int kVerdictWaitMs = 3000;
    // How long a paused engine keeps running before it is stopped.
    static constexpr int kParkAfterMs = 60000;
    // The wait before the item starts (tests shorten it).
    void setVerdictWait(int ms) { m_waitTimer.setInterval(ms); }
    explicit SceneItem(QQuickItem *parent = nullptr);

    QString itemDir() const { return m_itemDir; }
    void setItemDir(const QString &dir);
    QString assetsDir() const { return m_assetsDir; }
    void setAssetsDir(const QString &dir);
    QString outputName() const { return m_outputName; }
    void setOutputName(const QString &name);
    // The desktop the output shows (as the daemon's assignments name it): a shortcut launch is judged for that (output, desktop).
    QString desktop() const { return m_desktop; }
    void setDesktop(const QString &desktop);
    QString sceneEngine() const { return m_sceneEngine; }
    void setSceneEngine(const QString &mode);
    QString engineLabel() const { return m_engineLabel; }
    bool sandbox() const { return m_sandbox; }
    void setSandbox(bool sandbox);
    bool requireSandbox() const { return m_requireSandbox; }
    void setRequireSandbox(bool require);
    double renderScale() const { return m_renderScale; }
    void setRenderScale(double scale);

signals:
    // A late verdict: the Vulkan engine can draw this item in full and the item started before it was back.
    void engineUpgradeAvailable();
    void itemDirChanged();
    void assetsDirChanged();
    void outputNameChanged();
    void desktopChanged();
    void sceneEngineChanged();
    void engineLabelChanged();
    void sandboxChanged();
    void requireSandboxChanged();
    void renderScaleChanged();

protected:
    bool canStart() const override;
    QString configurationError() const override;
    BridgeLaunch buildLaunch(int bridgeFd, const QSize &size) const override;
    QSize bufferSize(const QSize &pixels) const override;
    bool handleHostLost(const QString &reason) override;
    void clientReady(BridgeClient *client) override;
    QVariantMap propertiesForEngine(const QVariantMap &properties) const override; // the shortcuts' applications, by name
    void propertiesApplied(const QVariantMap &properties) override; // the media of the session: cover and player state for the engine
    QString logName(const BridgeLaunch &launch) const override;

private:
    shortcut::ShortcutItem *shortcuts() const; // the shortcuts of the item in m_itemDir (none without usershortcut properties)
    void resetShortcuts();
    mutable std::unique_ptr<shortcut::ShortcutItem> m_shortcuts;
    mutable bool m_shortcutsNone = false; // the item has no usershortcut property
    QString m_itemDir;
    QString m_assetsDir;
    QString m_outputName;
    QString m_desktop;
    QString m_sceneEngine = QString(kDefaultSceneEngine);
    bool m_sandbox = kDefaultSandboxEngines;
    bool m_requireSandbox = kDefaultRequireSandbox;
    double m_renderScale = kDefaultSceneRenderScale; // the Vulkan engine's resolution
    // What the last launch chose (buildLaunch is const, the choice is bookkeeping about it).
    mutable SceneEngineChoice m_choice;
    mutable QString m_engineLabel;

    // The wait for a verdict before the first launch (canStart is const; the wait is bookkeeping about it).
    bool waitingForVerdict() const;
    void onVerdict(const QString &itemDir);
    void endVerdictWait();
    mutable bool m_waiting = false;
    mutable bool m_waitOver = false;      // the wait ended (verdict, failure or timeout): launch whatever is known
    mutable QString m_waitKey;
    mutable QTimer m_waitTimer;
    bool m_upgradeOffered = false;
};

} // namespace murasaki
