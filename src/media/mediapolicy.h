#pragma once

// The host side of the media bridge: what the host accepts from a player and from the URL a player names. Everything here is a
// pure function (no bus, no network, no files), so the rules can be tested. A player and its metadata are untrusted: any local
// process can own an org.mpris.MediaPlayer2.* name.

#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <vector>

namespace murasaki::media {

inline constexpr int kMaxTextBytes = 256;      // a title, artist, album... as UTF-8
inline constexpr int kMaxGenres = 8;
inline constexpr qint64 kMaxArtBytes = 8 * 1024 * 1024;  // a cover file or download
inline constexpr int kMaxArtSide = 8192;       // of the picture as encoded
inline constexpr qint64 kMaxArtPixels = 16 * 1000 * 1000;
inline constexpr int kMaxRawText = 4096;       // characters of a player's string looked at before cleaning
inline constexpr int kThumbnailSide = 512;     // the longer side of what is sent to the engine
inline constexpr int kMaxRedirects = 4;
inline constexpr int kDownloadTimeoutMs = 10000;
inline constexpr int kMinFetchGapMs = 2000;    // between two new https URLs
inline constexpr int kMinFileGapMs = 500;      // between two cover files read
inline constexpr int kDownloadTotalMs = 15000; // a download is aborted after this, whatever it is doing
inline constexpr int kCoverCacheSize = 8;

// A player as the host read it from the bus.
struct PlayerSnapshot {
    QString owner;          // the bus name that owns it (unique name)
    QString status;         // "Playing", "Paused", "Stopped" (anything else counts as stopped)
    QString title, artist, albumTitle, albumArtist;
    QStringList genres;
    double positionSec = 0.0;
    double rate = 1.0;
    double durationSec = 0.0;
    QString artUrl;
    qint64 playingSince = 0; // monotonic ms when it last became Playing; 0: it never played while followed
};

// UTF-8 text cut at kMaxTextBytes on a character boundary, with control characters dropped (tab, newline as a space).
QString cleanText(const QString &text);

// Which player to follow: the one Playing (the one that became Playing most recently if several), else the one that played last,
// else the first with a title (by bus name), else none (-1).
int choosePlayer(const std::vector<PlayerSnapshot> &players);

// The Media message JSON for a player, or for none (null). Strings are cleaned and bounded, numbers finite and clamped.
QByteArray mediaJson(const PlayerSnapshot *player);

// A cover address the host may read. `file://` names a path on this machine (no host, or "localhost"); `https://` a host with
// no credentials. Everything else (http, data, ftp, relative, empty) is refused.
struct ArtSource {
    enum class Kind { None, File, Https } kind = Kind::None;
    QString path;  // File
    QUrl url;      // Https
};
ArtSource classifyArtUrl(const QString &raw);

// An address a cover download must never go to: this host's own, the local network's and the ranges set apart for documentation
// and testing. IPv4: 0.0.0.0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24, 192.0.2/24, 192.168/16, 198.18/15,
// 198.51.100/24, 203.0.113/24, 224/4 and 240/4; IPv6: ::, ::1, fc00::/7, fe80::/10, fec0::/10, ff00::/8, 64:ff9b:1::/48 and
// 2001:db8::/32 whole, and by the IPv4 inside: an IPv4-mapped (::ffff:a.b.c.d), IPv4-compatible (::a.b.c.d), NAT64 (64:ff9b::/96),
// 6to4 (2002::/16) or Teredo (2001::/32: the server and the obfuscated client) address. The address is the RESOLVED
// one: the check is made on what the name resolved to, not on its text.
bool isBlockedAddress(const QHostAddress &address);

// A path a cover file may not come from: the kernel's and the devices' (/proc, /sys, /dev), whatever links lead there.
bool isForbiddenArtPath(const QString &canonicalPath);

} // namespace murasaki::media
