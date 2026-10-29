#pragma once

#include <QByteArray>
#include <QString>

namespace murasaki {

// What `murasaki-sandbox --report` says the system offers for confining a scene engine.
struct SandboxReport {
    bool valid = false;      // the helper ran and answered
    int landlockAbi = 0;     // 0: no Landlock
    bool tcpDeny = false;    // TCP can be refused (Landlock ABI 4 or a network namespace)
    bool scopes = false;     // abstract unix sockets and signals can be confined (ABI 6)
    bool bwrap = false;      // a mount namespace is available: the session bus is out of reach

    // Whether an engine started behind the helper is really confined: Landlock applies and TCP is refused.
    bool confines() const { return valid && landlockAbi >= 1 && tcpDeny; }
    // One line for people: "Landlock ABI 8, TCP refused, mount namespace: yes" or why nothing applies.
    QString summary() const;
};

// Parses the helper's line. An invalid report (valid false) for anything unreadable.
SandboxReport parseSandboxReport(const QByteArray &output);

// Runs `<helper> --report` (at most 15 s) and parses it. The answer is kept for the life of the process per helper file
// (size and modification time), since the system does not change under a running program. An empty helper path gives an
// invalid report.
SandboxReport querySandboxReport(const QString &helper);

} // namespace murasaki
