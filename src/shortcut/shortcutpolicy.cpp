#include "shortcut/shortcutpolicy.h"

#include <QChar>
#include <QList>
#include <QStringDecoder>

namespace murasaki::shortcut {

namespace {

// Characters that change how text is shown or that show as nothing: bidirectional controls and isolates, zero-width marks,
// the byte order mark, the soft hyphen.
bool hiddenFormat(char32_t c)
{
    return (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x206F)
        || c == 0x061C || c == 0xFEFF || c == 0x00AD || c == 0x180E
        || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) // variation selectors
        || (c >= 0xE0000 && c <= 0xE007F);                                // tag characters (they spell hidden text)
}

bool combining(char32_t c)
{
    const QChar::Category category = QChar::category(c);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining || category == QChar::Mark_Enclosing;
}

} // namespace

bool validKey(const QByteArray &key)
{
    if (key.isEmpty() || key.size() > kMaxKeyBytes) {
        return false;
    }
    for (const char c : key) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return false;
        }
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder(key);
    return !decoder.hasError() && !text.contains(QChar::ReplacementCharacter);
}

QString cleanDisplayName(const QString &name)
{
    QString out;
    bool space = false;
    int marks = 0; // consecutive combining marks kept (a stack of them is how text is made to overflow its line)
    const QList<uint> points = name.left(4096).toUcs4(); // whatever the caller handed over is bounded first
    for (const uint cp : points) {
        const char32_t c = cp;
        if (c == 0 || hiddenFormat(c)) {
            continue;
        }
        const bool blank = c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0xA0 || c == 0x2028 || c == 0x2029;
        if (!blank && (c < 0x20 || (c >= 0x7f && c <= 0x9f) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF)) {
            continue; // control, surrogate or out of range
        }
        if (blank) {
            space = !out.isEmpty();
            continue;
        }
        marks = combining(c) ? marks + 1 : 0;
        if (marks > 2) {
            continue;
        }
        if (space) {
            out += QLatin1Char(' ');
            space = false;
        }
        out += QString::fromUcs4(&c, 1);
        if (out.toUtf8().size() > kMaxDisplayNameBytes) {
            // too long: drop the character that went over (always on a character boundary) and stop
            out.chop(QString::fromUcs4(&c, 1).size());
            break;
        }
    }
    return out;
}

const char *verdictName(Verdict verdict)
{
    switch (verdict) {
    case Verdict::Allow: return "allowed";
    case Verdict::SwitchOff: return "the setting is off";
    case Verdict::UnknownKey: return "not a shortcut property of this wallpaper";
    case Verdict::Unassigned: return "no application assigned";
    case Verdict::NoRecentClick: return "no recent click";
    case Verdict::TooSoon: return "too soon after the last launch";
    case Verdict::SameAppTooSoon: return "the same application again too soon";
    case Verdict::RateLimit: return "too many launches";
    case Verdict::NotVisible: return "the wallpaper is not shown";
    }
    return "";
}

void LaunchLimiter::trim(qint64 nowMs)
{
    while (!m_times.empty() && (nowMs < m_times.front() || nowMs - m_times.front() >= 60000)) {
        m_times.pop_front();
    }
}

bool LaunchLimiter::allows(qint64 nowMs)
{
    trim(nowMs);
    return static_cast<int>(m_times.size()) < m_perMinute;
}

void LaunchLimiter::record(qint64 nowMs)
{
    trim(nowMs);
    m_times.push_back(nowMs);
}

Verdict GlobalLaunchPolicy::decide(qint64 nowMs, bool switchOn, const QString &desktopId)
{
    if (!switchOn) {
        return Verdict::SwitchOff;
    }
    if (desktopId.isEmpty()) {
        return Verdict::Unassigned;
    }
    if (m_any && desktopId == m_lastId && nowMs >= m_lastIdAt && nowMs - m_lastIdAt < m_limits.sameAppGapMs) {
        return Verdict::SameAppTooSoon;
    }
    if (!m_limiter.allows(nowMs)) {
        return Verdict::RateLimit;
    }
    m_limiter.record(nowMs);
    m_any = true;
    m_lastId = desktopId;
    m_lastIdAt = nowMs;
    return Verdict::Allow;
}

Verdict LaunchGate::decide(const LaunchRequest &r)
{
    if (!r.allowed) {
        return Verdict::SwitchOff;
    }
    if (!r.knownKey) {
        return Verdict::UnknownKey;
    }
    if (r.assignedId.isEmpty()) {
        return Verdict::Unassigned;
    }
    if (!r.visible) {
        return Verdict::NotVisible;
    }
    // a press the host saw, shortly before (a clock that went back counts as no press)
    if (!m_clicked || r.nowMs < m_lastClick || r.nowMs - m_lastClick > m_limits.clickWindowMs) {
        return Verdict::NoRecentClick;
    }
    if (m_launched && r.nowMs >= m_lastLaunch && r.nowMs - m_lastLaunch < m_limits.gapMs) {
        return Verdict::TooSoon;
    }
    if (m_launched && r.assignedId == m_lastId && r.nowMs >= m_lastIdAt && r.nowMs - m_lastIdAt < m_limits.sameAppGapMs) {
        return Verdict::SameAppTooSoon;
    }
    if (m_limiter && !m_limiter->allows(r.nowMs)) {
        return Verdict::RateLimit;
    }
    m_launched = true;
    m_lastLaunch = r.nowMs;
    m_lastId = r.assignedId;
    m_lastIdAt = r.nowMs;
    if (m_limiter) {
        m_limiter->record(r.nowMs);
    }
    m_clicked = false; // one press, one launch
    return Verdict::Allow;
}

} // namespace murasaki::shortcut
