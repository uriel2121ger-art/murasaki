#pragma once

// Downloads a cover for the host (never for the engine: the sandbox has no network). One request at a time, https only, with the
// limits of mediapolicy.h. Policy: the host name is resolved here and the download starts only when EVERY address it resolves to
// is a public one (isBlockedAddress); a redirect (https only, at most kMaxRedirects) goes through the same check. Residual risk,
// accepted because the URL comes from a player on this machine and not from a wallpaper: the connection resolves the name again, so
// a name that answers differently between the check and the connection (DNS rebinding) is not caught; a configured system proxy
// connects on the client's behalf.

#include "media/mediapolicy.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QUrl>


class QNetworkReply;

namespace murasaki::media {

class ArtFetcher : public QObject
{
    Q_OBJECT

public:
    explicit ArtFetcher(QObject *parent = nullptr);

    // Starts a download, abandoning the one in flight. Always ends with finished(url, bytes): the body as it came (at most kMaxArtBytes,
    // not decoded: the caller decodes it off the GUI thread), empty on any failure. A connection is direct (no system proxy), asks for
    // no content encoding, and is cut off after kDownloadTotalMs whatever it is doing.
    void fetch(const QUrl &url);
    void cancel();
    bool busy() const { return m_token != 0 && m_active; }

signals:
    void finished(const QUrl &url, const QByteArray &body);

private:
    void check(const QUrl &url, int hops, quint64 token);
    void request(const QUrl &url, int hops, quint64 token);
    void done(const QByteArray &body, quint64 token);

    QNetworkAccessManager m_manager;
    QNetworkReply *m_reply = nullptr;
    QTimer m_total; // the whole fetch, lookups and redirects included
    QUrl m_origin;
    quint64 m_token = 0; // what an answer must carry to count: a new fetch or a cancel changes it
    bool m_active = false;
};

} // namespace murasaki::media
