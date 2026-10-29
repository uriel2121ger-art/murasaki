#pragma once

#include <QHash>
#include <QWebEngineUrlSchemeHandler>

namespace murasaki {

// Serves web items under murasaki-item://<token>/<path>. A token maps to one item's content dir, and a request
// can only reach a regular file inside it (symlinks resolved), so a Workshop page cannot read anything else on
// disk. One handler serves one web profile, i.e. one shown wallpaper; `networkAllowed` says whether that wallpaper may use
// the internet: if not, its documents carry a Content-Security-Policy that closes the channels an interceptor cannot
// see (see requestStarted).
class ItemSchemeHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    explicit ItemSchemeHandler(bool networkAllowed = false, QObject *parent = nullptr)
        : QWebEngineUrlSchemeHandler(parent)
        , m_networkAllowed(networkAllowed)
    {
    }

    // Must run before the QGuiApplication is created.
    static void registerScheme();

    bool networkAllowed() const { return m_networkAllowed; }

    // The token for contentDir, created on first use.
    QString tokenFor(const QString &contentDir);

    void requestStarted(QWebEngineUrlRequestJob *job) override;

private:
    QHash<QString, QString> m_dirByToken;
    QHash<QString, QString> m_tokenByDir;
    bool m_networkAllowed;
};

} // namespace murasaki
