#include "steamcmdparser.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// What steamcmd prints, as the daemon reads it.
class TestSteamCmdParser : public QObject
{
    Q_OBJECT

private slots:
    void recognisesThePasswordPrompt();
    void recognisesTheSteamGuardPrompts();
    void aPartialLineThatIsNotAPromptIsNothing();
    void theMobileConfirmationIsALine();
    void loginSuccess();
    void loginFailures();
    void progressLines();
    void downloadResults();
    void ansiAndCarriageReturnsAreIgnored();
    void unrelatedLinesAreNothing();
    void splitsTerminalOutputIntoLines();
};

void TestSteamCmdParser::recognisesThePasswordPrompt()
{
    QCOMPARE(detectPrompt(u"password: "_s), SteamCmdPrompt::Password);
    QCOMPARE(detectPrompt(u"Password:"_s), SteamCmdPrompt::Password);
    QCOMPARE(detectPrompt(u"Logging in user 'bob' [U:1:1234] to Steam Public...password:"_s), SteamCmdPrompt::Password);
    QCOMPARE(detectPrompt(u"\u001b[1mpassword: \u001b[0m"_s), SteamCmdPrompt::Password);
}

void TestSteamCmdParser::recognisesTheSteamGuardPrompts()
{
    QCOMPARE(detectPrompt(u"Steam Guard code:"_s), SteamCmdPrompt::Guard);
    QCOMPARE(detectPrompt(u"Steam Guard code: "_s), SteamCmdPrompt::Guard);
    QCOMPARE(detectPrompt(u"Two-factor code:"_s), SteamCmdPrompt::Guard);
    QCOMPARE(detectPrompt(u"two-factor code: "_s), SteamCmdPrompt::Guard);
}

void TestSteamCmdParser::aPartialLineThatIsNotAPromptIsNothing()
{
    QCOMPARE(detectPrompt(QString()), SteamCmdPrompt::None);
    QCOMPARE(detectPrompt(u"Update state (0x61) downloading, progr"_s), SteamCmdPrompt::None);
    QCOMPARE(detectPrompt(u"Please enter your password in the field"_s), SteamCmdPrompt::None); // not at the end
    QCOMPARE(detectPrompt(u"Steam>"_s), SteamCmdPrompt::None);
}

void TestSteamCmdParser::theMobileConfirmationIsALine()
{
    const SteamCmdEvent event = parseSteamCmdLine(u"Please confirm the login in the Steam Mobile app on your phone."_s);
    QCOMPARE(event.kind, SteamCmdEvent::Prompt);
    QCOMPARE(event.prompt, SteamCmdPrompt::Confirm);
    // Steam Guard instructions before the code prompt are not prompts by themselves
    QCOMPARE(parseSteamCmdLine(u"code from that message."_s).kind, SteamCmdEvent::None);
}

void TestSteamCmdParser::loginSuccess()
{
    QCOMPARE(parseSteamCmdLine(u"Logging in user 'bob' [U:1:1234] to Steam Public...OK"_s).kind, SteamCmdEvent::LoggedIn);
    QCOMPARE(parseSteamCmdLine(u"Logged in OK"_s).kind, SteamCmdEvent::LoggedIn);
    QCOMPARE(parseSteamCmdLine(u"Waiting for user info...OK"_s).kind, SteamCmdEvent::LoggedIn);
    // "Waiting for client config...OK" alone is not yet a login
    QCOMPARE(parseSteamCmdLine(u"Waiting for client config...OK"_s).kind, SteamCmdEvent::None);
}

void TestSteamCmdParser::loginFailures()
{
    for (const QString &reason : {u"Invalid Password"_s, u"Two-factor code mismatch"_s, u"Account Logon Denied"_s,
                                  u"Rate Limit Exceeded"_s, u"Timeout"_s}) {
        const SteamCmdEvent event = parseSteamCmdLine(u"Logging in user 'bob' to Steam Public...FAILED (%1)"_s.arg(reason));
        QCOMPARE(event.kind, SteamCmdEvent::LoginFailed);
        QCOMPARE(event.text, reason);
    }
    const SteamCmdEvent bare = parseSteamCmdLine(u"FAILED (Invalid Password)"_s);
    QCOMPARE(bare.kind, SteamCmdEvent::LoginFailed);
    QCOMPARE(bare.text, u"Invalid Password"_s);
    QCOMPARE(parseSteamCmdLine(u"ERROR! Not logged in."_s).kind, SteamCmdEvent::LoginRequired);
    QCOMPARE(parseSteamCmdLine(u"Login Failure: Account Logon Denied"_s).kind, SteamCmdEvent::LoginFailed);
    // other wordings
    SteamCmdEvent code = parseSteamCmdLine(u"FAILED login with result code Invalid Password"_s);
    QCOMPARE(code.kind, SteamCmdEvent::LoginFailed);
    QCOMPARE(code.text, u"Invalid Password"_s);
    code = parseSteamCmdLine(u"ERROR (Rate Limit Exceeded)"_s);
    QCOMPARE(code.kind, SteamCmdEvent::LoginFailed);
    QCOMPARE(code.text, u"Rate Limit Exceeded"_s);
    QCOMPARE(parseSteamCmdLine(u"FAILED (No Connection)"_s).text, u"No Connection"_s);
    QCOMPARE(parseSteamCmdLine(u"Login Failure: Invalid Password"_s).text, u"Invalid Password"_s);
    // a download error is not a login failure
    QCOMPARE(parseSteamCmdLine(u"ERROR! Download item 5 failed (Failure)."_s).kind, SteamCmdEvent::DownloadFailed);
}

void TestSteamCmdParser::progressLines()
{
    const SteamCmdEvent event = parseSteamCmdLine(u"Update state (0x61) downloading, progress: 45.54 (12345 / 27123)"_s);
    QCOMPARE(event.kind, SteamCmdEvent::Progress);
    QVERIFY(qAbs(event.progress - 0.4554) < 1e-9);
    QCOMPARE(parseSteamCmdLine(u"Update state (0x61) downloading, progress: 100.00 (1 / 1)"_s).progress, 1.0);
    QCOMPARE(parseSteamCmdLine(u"Update state (0x5) validating, progress: 0.00 (0 / 10)"_s).progress, 0.0);
    // junk values are clamped
    QCOMPARE(parseSteamCmdLine(u"Update state (0x61) downloading, progress: 250.00 (1 / 1)"_s).progress, 1.0);
    QCOMPARE(parseSteamCmdLine(u"Downloading item 123456 ..."_s).kind, SteamCmdEvent::Started);
    QCOMPARE(parseSteamCmdLine(u"Downloading item 123456 ..."_s).itemId, u"123456"_s);
}

void TestSteamCmdParser::downloadResults()
{
    const SteamCmdEvent ok = parseSteamCmdLine(
        u"Success. Downloaded item 123456 to \"/home/u/.local/share/murasaki/steamcmd/steamapps/workshop/content/431960/123456\" (27123 bytes)"_s);
    QCOMPARE(ok.kind, SteamCmdEvent::Downloaded);
    QCOMPARE(ok.itemId, u"123456"_s);
    QVERIFY(ok.text.endsWith(u"/431960/123456"_s));
    const SteamCmdEvent failed = parseSteamCmdLine(u"ERROR! Download item 123456 failed (Access Denied)."_s);
    QCOMPARE(failed.kind, SteamCmdEvent::DownloadFailed);
    QCOMPARE(failed.itemId, u"123456"_s);
    QCOMPARE(failed.text, u"Access Denied"_s);
    QCOMPARE(parseSteamCmdLine(u"ERROR! Download item 7 failed (Failure)."_s).text, u"Failure"_s);
    QCOMPARE(parseSteamCmdLine(u"ERROR! Download item 7 failed (No subscription)."_s).text, u"No subscription"_s);
}

void TestSteamCmdParser::ansiAndCarriageReturnsAreIgnored()
{
    QCOMPARE(parseSteamCmdLine(u"\u001b[0m\u001b[1mLogged in OK\u001b[0m\r"_s).kind, SteamCmdEvent::LoggedIn);
    QCOMPARE(parseSteamCmdLine(u"\rUpdate state (0x61) downloading, progress: 10.00 (1 / 10)\r"_s).kind, SteamCmdEvent::Progress);
    QCOMPARE(stripTerminalCodes(u"a\u001b[31mb\u001b[0mc\u001b]0;title\u0007d"_s), u"abcd"_s);
}

void TestSteamCmdParser::unrelatedLinesAreNothing()
{
    for (const QString &line : {QString(), u"Redirecting stderr to '/x/logs/stderr.txt'"_s, u"Loading Steam API...OK"_s,
                                u"-- type 'quit' to exit --"_s, u"Steam>"_s, u"Checking for available updates..."_s})
        QCOMPARE(parseSteamCmdLine(line).kind, SteamCmdEvent::None);
}

void TestSteamCmdParser::splitsTerminalOutputIntoLines()
{
    LineSplitter splitter;
    // a read may end anywhere; \r\n, \n and a lone \r (progress redraws) all end a line
    QCOMPARE(splitter.feed("Logg").first, QStringList());
    QCOMPARE(splitter.pending(), u"Logg"_s);
    QCOMPARE(splitter.feed("ed in OK\r\nnext\nthird\rfourth").first, (QStringList{u"Logged in OK"_s, u"next"_s, u"third"_s}));
    QCOMPARE(splitter.pending(), u"fourth"_s);
    QCOMPARE(splitter.feed("\r\n").first, (QStringList{u"fourth"_s}));
    QCOMPARE(splitter.pending(), QString());
    // output is bounded: a stream without a newline cannot grow forever
    LineSplitter bounded;
    bounded.feed(QByteArray(100000, 'x'));
    QVERIFY(bounded.pending().size() <= LineSplitter::MaxPending);
}

QTEST_APPLESS_MAIN(TestSteamCmdParser)
#include "tst_steamcmdparser.moc"
