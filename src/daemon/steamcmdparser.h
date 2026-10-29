#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>

namespace murasaki {

// What SteamCMD waits for. Password and Guard are partial lines (the prompt has no newline); Confirm is a whole line
// ("Please confirm the login in the Steam Mobile app on your phone.").
enum class SteamCmdPrompt { None, Password, Guard, Confirm };

struct SteamCmdEvent {
    enum Kind {
        None,
        Prompt,         // `prompt` says which (only Confirm comes from a whole line)
        LoggedIn,
        LoginFailed,    // `text` is SteamCMD's reason ("Invalid Password"). Recognised wordings: "...FAILED (reason)",
                        // "FAILED login with result code reason", "ERROR (reason)" and "Login Failure: reason"
        LoginRequired,  // "ERROR! Not logged in.": the cached login is no good
        Started,        // "Downloading item <id> ..."
        Progress,       // `progress` 0..1
        Downloaded,     // `itemId`, `text` is the folder
        DownloadFailed, // `itemId`, `text` is SteamCMD's reason
    };
    Kind kind = None;
    SteamCmdPrompt prompt = SteamCmdPrompt::None;
    QString itemId;
    QString text;
    double progress = 0;
};

// Removes ANSI escape sequences and control characters other than newline.
QString stripTerminalCodes(const QString &text);

// The prompt that the text printed since the last newline ends with, or None.
SteamCmdPrompt detectPrompt(const QString &pendingText);

// Reads one finished line of output.
SteamCmdEvent parseSteamCmdLine(const QString &line);

// Cuts the byte stream of a terminal into lines (\r\n, \n and a lone \r, which progress redraws use, all end one),
// keeping the unfinished tail. Bytes are decoded as UTF-8; the tail is bounded.
class LineSplitter
{
public:
    static constexpr int MaxPending = 8192;

    // Returns the lines completed by `data`, and the same pending text pending() gives.
    QPair<QStringList, QString> feed(const QByteArray &data);
    QString pending() const { return m_pending; }
    // The unfinished tail as a line (at the end of the stream); empty when there is none.
    QString flush();

private:
    QString m_pending;
    bool m_lastWasCr = false;
};

} // namespace murasaki
