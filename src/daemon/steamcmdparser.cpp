#include "steamcmdparser.h"

#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {

QString stripTerminalCodes(const QString &text)
{
    static const QRegularExpression csi(u"\u001b\\[[0-9;?]*[ -/]*[@-~]"_s);
    static const QRegularExpression osc(u"\u001b\\][^\u0007\u001b]*(\u0007|\u001b\\\\)"_s);
    static const QRegularExpression other(u"\u001b[@-Z\\\\-_]"_s);
    QString out = text;
    out.remove(osc);
    out.remove(csi);
    out.remove(other);
    QString clean;
    clean.reserve(out.size());
    for (const QChar c : std::as_const(out)) {
        if (c == u'\n' || c == u'\r' || c == u'\t' || c.unicode() >= 0x20)
            clean.append(c);
    }
    return clean;
}

SteamCmdPrompt detectPrompt(const QString &pendingText)
{
    const QString text = stripTerminalCodes(pendingText).trimmed();
    if (text.isEmpty())
        return SteamCmdPrompt::None;
    if (text.endsWith(u"Steam Guard code:", Qt::CaseInsensitive) || text.endsWith(u"Two-factor code:", Qt::CaseInsensitive))
        return SteamCmdPrompt::Guard;
    if (text.endsWith(u"password:", Qt::CaseInsensitive))
        return SteamCmdPrompt::Password;
    return SteamCmdPrompt::None;
}

SteamCmdEvent parseSteamCmdLine(const QString &rawLine)
{
    const QString line = stripTerminalCodes(rawLine).trimmed();
    SteamCmdEvent event;
    if (line.isEmpty())
        return event;

    static const QRegularExpression progress(u"progress:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*\\("_s);
    static const QRegularExpression downloaded(u"^Success\\. Downloaded item (\\d+) to \\\"(.*)\\\"( \\(\\d+ bytes\\))?"_s);
    static const QRegularExpression downloadFailed(u"^ERROR! Download item (\\d+) failed \\(([^)]*)\\)"_s);
    static const QRegularExpression started(u"^Downloading item (\\d+)"_s);
    static const QRegularExpression failed(u"FAILED \\(([^)]*)\\)"_s);
    static const QRegularExpression failedWithCode(u"^FAILED login with result code (.+)$"_s);
    static const QRegularExpression errorWord(u"^ERROR \\(([^)]*)\\)"_s);

    if (const auto m = downloaded.match(line); m.hasMatch()) {
        event.kind = SteamCmdEvent::Downloaded;
        event.itemId = m.captured(1);
        event.text = m.captured(2);
        return event;
    }
    if (const auto m = downloadFailed.match(line); m.hasMatch()) {
        event.kind = SteamCmdEvent::DownloadFailed;
        event.itemId = m.captured(1);
        event.text = m.captured(2);
        return event;
    }
    if (const auto m = started.match(line); m.hasMatch()) {
        event.kind = SteamCmdEvent::Started;
        event.itemId = m.captured(1);
        return event;
    }
    if (line.contains(u"Update state (")) {
        if (const auto m = progress.match(line); m.hasMatch()) {
            event.kind = SteamCmdEvent::Progress;
            event.progress = std::clamp(m.captured(1).toDouble() / 100.0, 0.0, 1.0);
            return event;
        }
    }
    if (line.contains(u"confirm the login in the Steam Mobile app", Qt::CaseInsensitive)) {
        event.kind = SteamCmdEvent::Prompt;
        event.prompt = SteamCmdPrompt::Confirm;
        return event;
    }
    if (line.startsWith(u"ERROR! Not logged in"_s)) {
        event.kind = SteamCmdEvent::LoginRequired;
        return event;
    }
    if (line.startsWith(u"Login Failure:"_s)) {
        event.kind = SteamCmdEvent::LoginFailed;
        event.text = line.mid(int(sizeof("Login Failure:")) - 1).trimmed();
        return event;
    }
    // More ways SteamCMD words a refused login: "FAILED login with result code X" and "ERROR (X)".
    if (const auto m = failedWithCode.match(line); m.hasMatch()) {
        event.kind = SteamCmdEvent::LoginFailed;
        event.text = m.captured(1);
        return event;
    }
    if (const auto m = errorWord.match(line); m.hasMatch()) {
        event.kind = SteamCmdEvent::LoginFailed;
        event.text = m.captured(1);
        return event;
    }
    if (const auto m = failed.match(line); m.hasMatch() && (line.startsWith(u"FAILED") || line.contains(u"Logging in"))) {
        event.kind = SteamCmdEvent::LoginFailed;
        event.text = m.captured(1);
        return event;
    }
    if (line == u"Logged in OK" || line.contains(u"Waiting for user info...OK")
        || (line.contains(u"Logging in user") && line.endsWith(u"...OK"))) {
        event.kind = SteamCmdEvent::LoggedIn;
        return event;
    }
    return event;
}

QPair<QStringList, QString> LineSplitter::feed(const QByteArray &data)
{
    QStringList lines;
    const QString text = QString::fromUtf8(data);
    for (const QChar c : text) {
        if (c == u'\n' && m_lastWasCr) { // the second half of \r\n
            m_lastWasCr = false;
            continue;
        }
        m_lastWasCr = c == u'\r';
        if (c == u'\n' || c == u'\r') {
            lines.append(m_pending);
            m_pending.clear();
        } else {
            m_pending.append(c);
            if (m_pending.size() > MaxPending)
                m_pending = m_pending.right(MaxPending / 2);
        }
    }
    return {lines, m_pending};
}

QString LineSplitter::flush()
{
    QString tail = std::exchange(m_pending, QString());
    m_lastWasCr = false;
    return tail;
}

} // namespace murasaki
