#include "media/mediapolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace murasaki::media {

namespace {

bool blockedV4(quint32 a)
{
    const quint32 o1 = a >> 24, o2 = (a >> 16) & 0xFF;
    const quint32 o3 = (a >> 8) & 0xFF;
    return o1 == 0 || o1 == 10 || (o1 == 100 && (o2 & 0xC0) == 64) || o1 == 127 || (o1 == 169 && o2 == 254) || (o1 == 172 && (o2 & 0xF0) == 16)
        || (o1 == 192 && o2 == 0 && (o3 == 0 || o3 == 2)) || (o1 == 192 && o2 == 168) || (o1 == 198 && (o2 & 0xFE) == 18)
        || (o1 == 198 && o2 == 51 && o3 == 100) || (o1 == 203 && o2 == 0 && o3 == 113) || o1 >= 224;
}

double clamped(double v, double lo, double hi, double fallback)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

} // namespace

QString cleanText(const QString &text)
{
    const QString bounded = text.left(kMaxRawText); // a hostile string is not walked to its end
    QString clean;
    clean.reserve(bounded.size());
    for (const QChar c : bounded) {
        if (c == u'\t' || c == u'\n' || c == u'\r') {
            clean += u' ';
        } else if (c.unicode() >= 0x20 && c.unicode() != 0x7F && !(c.unicode() >= 0x80 && c.unicode() < 0xA0)) {
            clean += c;
        }
    }
    clean = clean.trimmed();
    // cut by UTF-8 size on a character boundary (a surrogate pair stays whole: QString::toUtf8 of a half would be a replacement)
    QByteArray bytes = clean.toUtf8();
    if (bytes.size() <= kMaxTextBytes) {
        return clean;
    }
    int cut = kMaxTextBytes;
    while (cut > 0 && (static_cast<unsigned char>(bytes[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return QString::fromUtf8(bytes.constData(), cut);
}

int choosePlayer(const std::vector<PlayerSnapshot> &players)
{
    int best = -1;
    const auto better = [&](size_t i, auto key) { return best < 0 || key(players[i]) > key(players[static_cast<size_t>(best)]); };
    for (size_t i = 0; i < players.size(); ++i) {
        if (players[i].status == QLatin1String("Playing") && better(i, [](const PlayerSnapshot &p) { return p.playingSince; })) {
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) {
        return best;
    }
    for (size_t i = 0; i < players.size(); ++i) {
        if (players[i].playingSince > 0 && better(i, [](const PlayerSnapshot &p) { return p.playingSince; })) {
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) {
        return best;
    }
    for (size_t i = 0; i < players.size(); ++i) {
        if (!players[i].title.isEmpty() && (best < 0 || players[i].owner < players[static_cast<size_t>(best)].owner)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

QByteArray mediaJson(const PlayerSnapshot *p)
{
    QJsonObject o;
    if (!p) {
        o.insert(QStringLiteral("enabled"), false);
        return QJsonDocument(o).toJson(QJsonDocument::Compact);
    }
    o.insert(QStringLiteral("enabled"), true);
    o.insert(QStringLiteral("state"), p->status == QLatin1String("Playing") ? QStringLiteral("playing")
                                       : p->status == QLatin1String("Paused") ? QStringLiteral("paused") : QStringLiteral("stopped"));
    o.insert(QStringLiteral("title"), cleanText(p->title));
    o.insert(QStringLiteral("artist"), cleanText(p->artist));
    o.insert(QStringLiteral("albumTitle"), cleanText(p->albumTitle));
    o.insert(QStringLiteral("albumArtist"), cleanText(p->albumArtist));
    QJsonArray genres;
    for (const QString &g : p->genres) {
        if (genres.size() >= kMaxGenres) {
            break;
        }
        genres.append(cleanText(g));
    }
    o.insert(QStringLiteral("genres"), genres);
    o.insert(QStringLiteral("position"), clamped(p->positionSec, 0.0, 86400.0, 0.0));
    o.insert(QStringLiteral("rate"), clamped(p->rate, 0.0, 16.0, 1.0));
    o.insert(QStringLiteral("duration"), clamped(p->durationSec, 0.0, 86400.0, 0.0));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

ArtSource classifyArtUrl(const QString &raw)
{
    ArtSource none;
    if (raw.isEmpty() || raw.size() > 2048) {
        return none;
    }
    const QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative()) {
        return none;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("file")) {
        const QString host = url.host();
        if (!host.isEmpty() && host != QLatin1String("localhost")) {
            return none;
        }
        const QString path = url.toLocalFile();
        if (path.isEmpty() || !path.startsWith(u'/')) {
            return none;
        }
        ArtSource s;
        s.kind = ArtSource::Kind::File;
        s.path = path;
        return s;
    }
    if (scheme == QLatin1String("https")) {
        if (url.host().isEmpty() || !url.userInfo().isEmpty()) {
            return none;
        }
        ArtSource s;
        s.kind = ArtSource::Kind::Https;
        s.url = url;
        return s;
    }
    return none;
}

bool isBlockedAddress(const QHostAddress &address)
{
    if (address.isNull()) {
        return true;
    }
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        return blockedV4(address.toIPv4Address());
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol) {
        return true;
    }
    const Q_IPV6ADDR a = address.toIPv6Address();
    const auto v4 = [&](int at) {
        return (quint32{a[at]} << 24) | (quint32{a[at + 1]} << 16) | (quint32{a[at + 2]} << 8) | quint32{a[at + 3]};
    };
    bool firstTenZero = true, firstEightZero = true;
    for (int i = 0; i < 10; ++i) {
        firstTenZero = firstTenZero && a[i] == 0;
    }
    for (int i = 0; i < 12; ++i) {
        firstEightZero = firstEightZero && a[i] == 0;
    }
    if (firstEightZero) {
        return blockedV4(v4(12)); // ::, ::1 and IPv4-compatible (::a.b.c.d): 0.0.0.0/8 covers the first two
    }
    if (firstTenZero && a[10] == 0xFF && a[11] == 0xFF) {
        return blockedV4(v4(12)); // IPv4-mapped
    }
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xFF && a[3] == 0x9B) {
        bool zero = true;
        for (int i = 4; i < 12; ++i) {
            zero = zero && a[i] == 0;
        }
        if (zero) {
            return blockedV4(v4(12)); // NAT64 64:ff9b::/96
        }
    }
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xFF && a[3] == 0x9B && a[4] == 0x00 && a[5] == 0x01) {
        return true; // 64:ff9b:1::/48 (local-use NAT64)
    }
    if (a[0] == 0x20 && a[1] == 0x02) {
        return blockedV4(v4(2)); // 6to4: the IPv4 address follows the prefix
    }
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x00 && a[3] == 0x00) {
        return blockedV4(v4(4)) || blockedV4(~v4(12)); // Teredo: the server, and the client (stored inverted)
    }
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0D && a[3] == 0xB8) {
        return true; // 2001:db8::/32, documentation
    }
    return (a[0] & 0xFE) == 0xFC                 // fc00::/7
        || (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) // fec0::/10 (site-local, deprecated)
        || (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) // fe80::/10
        || a[0] == 0xFF;                           // ff00::/8
}

bool isForbiddenArtPath(const QString &canonicalPath)
{
    for (const char *prefix : {"/proc", "/sys", "/dev"}) {
        const QString p = QString::fromLatin1(prefix);
        if (canonicalPath == p || canonicalPath.startsWith(p + u'/')) {
            return true;
        }
    }
    return false;
}

} // namespace murasaki::media
