#include "autostart.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// Never talks to the real systemd: every test installs a fake runner first.
class TestAutostart : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { autostart::setCommandRunner({}); }

    void isEnabledFollowsExitCode();
    void setEnabledBuildsCommands();
    void setEnabledReportsErrors();
    void setEnabledFlagsCouldNotRun();
    void realRunnerUsesSystemctlFromPath();
    void realRunnerMapsFailures();
};

void TestAutostart::isEnabledFollowsExitCode()
{
    int code = 0;
    QStringList seen;
    int seenTimeout = -1;
    autostart::setCommandRunner([&](const QStringList &args, int timeout, bool) {
        seen = args;
        seenTimeout = timeout;
        return code;
    });
    QVERIFY(autostart::isEnabled());
    QCOMPARE(seen, QStringList({u"--user"_s, u"is-enabled"_s, u"--quiet"_s, u"murasaki-surface.service"_s}));
    QVERIFY(seenTimeout > 0);
    code = 1;
    QVERIFY(!autostart::isEnabled());
    code = -1;
    QVERIFY(!autostart::isEnabled());
}

void TestAutostart::setEnabledBuildsCommands()
{
    QList<QStringList> calls;
    autostart::setCommandRunner([&](const QStringList &args, int, bool) {
        calls << args;
        return 0;
    });
    QString error;
    QVERIFY(autostart::setEnabled(true, &error));
    QVERIFY(autostart::setEnabled(false));
    QCOMPARE(calls.value(0), QStringList({u"--user"_s, u"enable"_s, u"--now"_s, u"murasaki-surface.service"_s}));
    QCOMPARE(calls.value(1), QStringList({u"--user"_s, u"disable"_s, u"--now"_s, u"murasaki-surface.service"_s}));
    QVERIFY(error.isEmpty());
}

void TestAutostart::setEnabledReportsErrors()
{
    int code = 1;
    autostart::setCommandRunner([&](const QStringList &, int, bool) { return code; });
    QString error;
    QVERIFY(!autostart::setEnabled(true, &error));
    QVERIFY(error.contains(u"exit code 1"_s));
    code = -1;
    QVERIFY(!autostart::setEnabled(true, &error));
    QCOMPARE(error, u"cannot run systemctl"_s);
    QVERIFY(!autostart::setEnabled(true)); // null error pointer is fine
}

void TestAutostart::setEnabledFlagsCouldNotRun()
{
    int code = 1;
    autostart::setCommandRunner([&](const QStringList &, int, bool) { return code; });
    bool couldNotRun = true;
    QVERIFY(!autostart::setEnabled(true, nullptr, &couldNotRun));
    QVERIFY(!couldNotRun);
    code = -1;
    QVERIFY(!autostart::setEnabled(true, nullptr, &couldNotRun));
    QVERIFY(couldNotRun);
    code = 0;
    QVERIFY(autostart::setEnabled(true, nullptr, &couldNotRun));
    QVERIFY(!couldNotRun);
}

namespace {

// Puts a fake systemctl first in PATH: it appends its arguments to <dir>/args and exits with the number
// stored in <dir>/exit (0 if absent). The real systemd is never involved.
class FakeSystemctl
{
public:
    FakeSystemctl()
    {
        const QString script = m_dir.filePath(u"systemctl"_s);
        QFile file(script);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot write fake systemctl");
        file.write("#!/bin/sh\necho \"$*\" >> \"" + m_dir.path().toUtf8() + "/args\"\n"
                   "code=0\n[ -f \"" + m_dir.path().toUtf8() + "/exit\" ] && code=$(cat \"" + m_dir.path().toUtf8()
                   + "/exit\")\nexit $code\n");
        file.close();
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
        m_oldPath = qgetenv("PATH");
        qputenv("PATH", m_dir.path().toUtf8() + ':' + m_oldPath);
    }
    ~FakeSystemctl() { qputenv("PATH", m_oldPath); }
    void setExit(int code) { writeFile(u"exit"_s, QByteArray::number(code)); }
    QStringList calls() const
    {
        QFile file(m_dir.filePath(u"args"_s));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
    }

private:
    void writeFile(const QString &name, const QByteArray &data)
    {
        QFile file(m_dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
            qFatal("cannot write %s", qPrintable(name));
    }
    QTemporaryDir m_dir;
    QByteArray m_oldPath;
};

} // namespace

void TestAutostart::realRunnerUsesSystemctlFromPath()
{
    FakeSystemctl fake;
    QVERIFY(autostart::isEnabled());
    QVERIFY(autostart::setEnabled(true));
    QVERIFY(autostart::setEnabled(false));
    QCOMPARE(fake.calls(), QStringList({u"--user is-enabled --quiet murasaki-surface.service"_s,
                                         u"--user enable --now murasaki-surface.service"_s,
                                         u"--user disable --now murasaki-surface.service"_s}));
}

void TestAutostart::realRunnerMapsFailures()
{
    FakeSystemctl fake;
    fake.setExit(3);
    QVERIFY(!autostart::isEnabled());
    QString error;
    bool couldNotRun = true;
    QVERIFY(!autostart::setEnabled(true, &error, &couldNotRun));
    QVERIFY(!couldNotRun);
    QVERIFY(error.contains(u"exit code 3"_s));

    // No systemctl at all: PATH points only at an empty dir.
    const QByteArray oldPath = qgetenv("PATH");
    QTemporaryDir empty;
    qputenv("PATH", empty.path().toUtf8());
    QVERIFY(!autostart::isEnabled());
    QVERIFY(!autostart::setEnabled(true, &error, &couldNotRun));
    QVERIFY(couldNotRun);
    QCOMPARE(error, u"cannot run systemctl"_s);
    qputenv("PATH", oldPath);
}

QTEST_MAIN(TestAutostart)
#include "tst_autostart.moc"
