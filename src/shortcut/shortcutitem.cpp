#include "shortcut/shortcutitem.h"

#include "bridgeclient.h"
#include "shortcut/iconrender.h"
#include "shortcut/projectkeys.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QThreadPool>
#include <QTimer>

namespace murasaki::shortcut {

namespace {

// The thread that decodes icons: one, our own (not the global pool), and the object that receives its answers on the GUI thread. Made on the
// GUI thread, under the application object; it waits for its worker when the application goes.
class IconWorker : public QObject
{
public:
    explicit IconWorker(QObject *parent) : QObject(parent)
    {
        pool.setMaxThreadCount(1);
    }
    ~IconWorker() override { pool.waitForDone(); }
    static IconWorker &shared()
    {
        static IconWorker *worker = new IconWorker(QCoreApplication::instance());
        return *worker;
    }
    QThreadPool pool;
};

} // namespace

ShortcutHub &ShortcutHub::shared()
{
    static ShortcutHub hub;
    return hub;
}

ShortcutEnvironment systemShortcutEnvironment()
{
    ShortcutEnvironment env;
    env.roots = applicationRoots();
    env.desktop = systemDesktopEnvironment();
    env.iconRoots = iconRoots();
    env.themeName = QIcon::themeName(); // (QIcon belongs to the GUI thread: asked here, the worker gets strings)
    env.themePaths = QIcon::themeSearchPaths();
    env.nowMs = [] {
        static const QElapsedTimer clock = [] {
            QElapsedTimer t;
            t.start();
            return t;
        }();
        return clock.elapsed();
    };
    return env;
}

ShortcutItem::ShortcutItem(QString itemDir, ShortcutEnvironment env, LaunchLimiter *limiter, QObject *parent)
    : QObject(parent), m_itemDir(std::move(itemDir)), m_env(std::move(env)), m_keys(userShortcutKeys(m_itemDir)), m_gate(limiter)
{
    if (!m_env.nowMs) {
        m_env.nowMs = [] { return qint64{0}; };
    }
}

QVariantMap ShortcutItem::engineProperties(const QVariantMap &properties) const
{
    QVariantMap out = properties;
    for (const QString &key : m_keys) {
        const auto it = properties.constFind(key);
        if (it == properties.constEnd() || it->typeId() != QMetaType::QString) {
            continue; // not given (the engine has its own default) or not a string: left as it is
        }
        const QString id = it->toString();
        QString name; // an id that is not installed, or not shown here, is "nothing chosen"
        if (!id.isEmpty()) {
            if (const auto entry = findApplication(m_env.roots, id, m_env.desktop)) {
                name = entry->name;
            }
        }
        out.insert(key, name);
    }
    return out;
}

void ShortcutItem::assignmentsChanged(const QVariantMap &properties)
{
    QHash<QString, QString> now;
    for (const QString &key : m_keys) {
        const auto it = properties.constFind(key);
        now.insert(key, it != properties.constEnd() && it->typeId() == QMetaType::QString ? it->toString() : QString());
    }
    if (now == m_assigned && !m_entries.isEmpty()) {
        return;
    }
    m_assigned = now;
    ++m_epoch;
    refreshEntries();
    pushIcons();
}

void ShortcutItem::refreshEntries()
{
    m_entries.clear();
    for (auto it = m_assigned.cbegin(); it != m_assigned.cend(); ++it) {
        if (it.value().isEmpty()) {
            continue;
        }
        if (const auto entry = findApplication(m_env.roots, it.value(), m_env.desktop)) {
            m_entries.insert(it.key(), *entry);
        }
    }
}

void ShortcutItem::attach(BridgeClient *client)
{
    m_client = client;
    m_sentId.clear(); // a new engine has none
    m_lastSent.clear();
    connect(client, &BridgeClient::shortcutLaunchRequested, this, &ShortcutItem::request);
    pushIcons();
}

void ShortcutItem::noteClick()
{
    m_gate.noteClick(m_env.nowMs());
}

Verdict ShortcutItem::decide(const QString &key)
{
    LaunchRequest r;
    r.nowMs = m_env.nowMs();
    r.allowed = ShortcutHub::shared().launchAllowed();
    r.knownKey = m_keys.contains(key);
    r.assignedId = m_entries.contains(key) ? m_entries.value(key).id : QString(); // a stored id that no longer resolves is nothing
    r.visible = !m_visible || m_visible();
    return m_gate.decide(r);
}

void ShortcutItem::request(const QByteArray &name)
{
    const QString key = QString::fromUtf8(name);
    const Verdict verdict = decide(key);
    if (verdict == Verdict::Allow) {
        emit launchApproved(m_itemDir, key);
        return;
    }
    const QString reason = QString::fromLatin1(verdictName(verdict));
    if (m_told.size() < 16 && !m_told.contains(reason)) { // said once per reason
        m_told.insert(reason);
        qInfo("murasaki: shortcut %s of %s not started: %s", qPrintable(key.left(64)), qPrintable(m_itemDir.right(64)), qPrintable(reason));
    }
    emit launchRefused(key, reason);
}

void ShortcutItem::pushIcons()
{
    if (!m_client || !m_client->isReady() || !m_client->supportsShortcut()) {
        return;
    }
    for (const QString &key : std::as_const(m_keys)) {
        const QString wanted = m_entries.contains(key) ? m_entries.value(key).id : QString();
        if (m_sentId.value(key) == wanted && m_sentId.contains(key)) {
            continue;
        }
        if (wanted.isEmpty() && !m_sentId.contains(key)) {
            continue; // nothing was sent and nothing is wanted: the engine's default stands
        }
        sendIcon(key);
    }
}

void ShortcutItem::sendIcon(const QString &key)
{
    // at most one icon a second for a slot: a settings page that is being changed does not flood the socket
    QElapsedTimer &last = m_lastSent[key];
    if (last.isValid() && last.elapsed() < 1000) {
        if (!m_pending.contains(key)) {
            m_pending.insert(key);
            QPointer<ShortcutItem> self(this);
            QTimer::singleShot(static_cast<int>(1000 - last.elapsed()) + 10, this, [self, key] {
                if (self) {
                    self->m_pending.remove(key);
                    self->pushIcons();
                }
            });
        }
        return;
    }
    last.start();
    const uint32_t generation = ++m_generation[key];
    const quint64 epoch = m_epoch;
    const QString id = m_entries.contains(key) ? m_entries.value(key).id : QString();
    if (id.isEmpty()) {
        m_sentId.insert(key, QString());
        m_client->sendShortcutIcon(key.toUtf8(), 0, 0, {}, generation); // cleared
        return;
    }
    if (const auto cached = m_icons.constFind(id); cached != m_icons.constEnd()) {
        m_sentId.insert(key, id);
        m_client->sendShortcutIcon(key.toUtf8(), kIconSide, kIconSide, *cached, generation);
        return;
    }
    const QString spec = m_entries.value(key).icon.isEmpty() ? QStringLiteral("application-x-executable") : m_entries.value(key).icon;
    const auto render = [spec, roots = m_env.iconRoots, theme = m_env.themeName, paths = m_env.themePaths]() -> std::optional<QByteArray> {
        std::optional<QImage> image;
        if (isIconPath(spec)) {
            image = loadIconFile(spec, roots);
        } else if (const auto file = findThemeIcon(spec, theme, paths)) {
            QStringList allowed = roots;
            for (const QString &p : paths) {
                if (const QString canonical = QFileInfo(p).canonicalFilePath(); !canonical.isEmpty()) {
                    allowed << canonical;
                }
            }
            image = loadThemeIconFile(*file, allowed);
        }
        return image ? fitIcon(*image) : std::nullopt;
    };
    if (!m_env.useThreads) {
        onIcon(key, id, generation, epoch, render());
        return;
    }
    // Decoding (and the icon theme, which may parse an SVG) runs on a worker thread of our own, one job at a time: a read that hangs holds
    // that one. The answer goes to the GUI thread through an object that lives there (never through this item, which may be gone): the
    // item is checked there.
    IconWorker &worker = IconWorker::shared();
    QPointer<ShortcutItem> self(this);
    QElapsedTimer asked;
    asked.start();
    worker.pool.start([&worker, self, key, id, generation, epoch, render, asked] {
        std::optional<QByteArray> rgba = render();
        QMetaObject::invokeMethod(&worker, [self, key, id, generation, epoch, asked, rgba = std::move(rgba)]() mutable {
            if (!self) {
                return;
            }
            if (asked.elapsed() > 1000) {
                rgba.reset(); // too slow: no icon (the slot keeps the scene's default)
            }
            self->onIcon(key, id, generation, epoch, std::move(rgba));
        }, Qt::QueuedConnection);
    });
}

void ShortcutItem::onIcon(const QString &key, const QString &id, uint32_t generation, quint64 epoch, std::optional<QByteArray> rgba)
{
    if (epoch != m_epoch || !m_client) {
        return; // the assignment changed meanwhile: the newer one sends its own
    }
    if (rgba) {
        if (m_icons.size() >= kMaxSlots) {
            m_icons.clear();
        }
        m_icons.insert(id, *rgba);
        m_sentId.insert(key, id);
        m_client->sendShortcutIcon(key.toUtf8(), kIconSide, kIconSide, *rgba, generation);
        return;
    }
    // no usable icon: the slot keeps the scene's own default (and the engine is told if it had another)
    const bool hadOne = !m_sentId.value(key).isEmpty();
    m_sentId.insert(key, id);
    if (hadOne) {
        m_client->sendShortcutIcon(key.toUtf8(), 0, 0, {}, generation);
    }
}

} // namespace murasaki::shortcut
