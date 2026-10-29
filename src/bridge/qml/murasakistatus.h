#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

namespace murasaki {

// What the daemon says one output shows, as far as the Plasma wallpaper package's placeholder underlay cares.
struct OutputInfo {
    QUrl preview;     // file URL of the item's preview; empty when unknown, unassigned or without one
    QString itemId;
    QString kind;     // video | web | scene (the item's kind); empty when nothing is assigned
    bool operator==(const OutputInfo &) const = default;
};

// The one resolver of a process. It asks `ResolveAssignment(output, desktop)` over D-Bus without blocking for
// every output somebody watches, drops replies that a newer request has superseded, and asks again when the
// daemon says assignments or items changed, when the desktop changes and when the daemon (re)appears. It
// never auto-starts the daemon, and with no daemon the infos stay empty without a log line: a wallpaper must
// start without one. Not a QML type: MurasakiStatus and MurasakiOutputStatus are its two faces.
class OutputResolver : public QObject
{
    Q_OBJECT

public:
    static OutputResolver *instance();

    QString desktop() const { return m_desktop; }
    void setDesktop(const QString &desktop);

    void watch(const QString &output);   // counted; the first watch of an output starts a lookup
    void unwatch(const QString &output);
    OutputInfo info(const QString &output) const { return m_info.value(output); }

signals:
    void desktopChanged();
    void infoChanged(const QString &output);

private slots:
    void resolveAll();
    void onItemsChanged(const QStringList &added, const QStringList &changed, const QStringList &removed);

private:
    explicit OutputResolver(QObject *parent = nullptr);
    void resolve(const QString &output);
    void apply(const QString &output, const OutputInfo &info);

    QString m_desktop;
    QHash<QString, int> m_watchers;
    QHash<QString, OutputInfo> m_info;
    QHash<QString, quint64> m_requests; // newest request per output, older replies are dropped
    quint64 m_nextRequest = 0;
};

// Settings shared by every MurasakiOutputStatus.
class MurasakiStatus : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // Desktop the assignments are resolved for; "*" (the default) is the wildcard desktop.
    Q_PROPERTY(QString desktop READ desktop WRITE setDesktop NOTIFY desktopChanged)

public:
    explicit MurasakiStatus(QObject *parent = nullptr);
    QString desktop() const;
    void setDesktop(const QString &desktop);

    // Starts the Murasaki app, detached: `hint` (the package knows where its install put it) if it is executable,
    // else the path this module was built for, else `murasaki` from PATH. False if none can be started. For the
    // wallpaper's "Open Murasaki" button.
    Q_INVOKABLE bool openMurasaki(const QString &hint = {});

signals:
    void desktopChanged();
};

// What `output` shows: `MurasakiOutputStatus { output: "DP-1" }` and bind to preview / itemId / kind, which
// notify when the daemon's answer arrives or changes.
class MurasakiOutputStatus : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(QUrl preview READ preview NOTIFY infoChanged)
    Q_PROPERTY(QString itemId READ itemId NOTIFY infoChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY infoChanged)

public:
    explicit MurasakiOutputStatus(QObject *parent = nullptr);
    ~MurasakiOutputStatus() override;

    QString output() const { return m_output; }
    void setOutput(const QString &output);
    QUrl preview() const { return info().preview; }
    QString itemId() const { return info().itemId; }
    QString kind() const { return info().kind; }

signals:
    void outputChanged();
    void infoChanged();

private:
    OutputInfo info() const { return OutputResolver::instance()->info(m_output); }

    QString m_output;
};

} // namespace murasaki
