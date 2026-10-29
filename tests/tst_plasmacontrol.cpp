#include "plasmacontrol.h"

#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki::plasma;

namespace {

// A fake plasmashell: a list of desktops whose plugin the "switch" script changes. The script itself is not run
// (ScriptRunsInAJsEngine does that); the fake recognises it by its shape.
class FakeBackend : public Backend
{
public:
    QList<Desktop> desktops;
    QStringList scripts;
    QStringList presenters;
    bool plasmaShellDown = false;
    bool ignoreSwitch = false;  // a script that does nothing
    bool presenterFails = false;
    QString currentPresenter = u"auto"_s;
    QString garbage;            // when set, the query answers this

    bool evaluate(const QString &script, QString *output, QString *error) override
    {
        scripts << script;
        if (plasmaShellDown) {
            *error = u"org.kde.plasmashell is not running"_s;
            return false;
        }
        if (script == queryScript()) {
            if (!garbage.isEmpty()) {
                *output = garbage;
                return true;
            }
            QJsonArray array;
            for (const Desktop &d : std::as_const(desktops))
                array.append(QJsonObject{{u"id"_s, d.id}, {u"plugin"_s, d.plugin}});
            *output = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) + u'\n';
            return true;
        }
        if (!ignoreSwitch) {
            // var map = {...}; ...
            const int start = script.indexOf(u'{'), end = script.indexOf(u"};"_s);
            const QJsonObject map = QJsonDocument::fromJson(script.mid(start, end - start + 1).toUtf8()).object();
            for (Desktop &d : desktops) {
                if (map.contains(d.id))
                    d.plugin = map.value(d.id).toString();
            }
        }
        output->clear();
        return true;
    }
    bool presenter(QString *value, QString *) override
    {
        *value = currentPresenter;
        return true;
    }
    bool setPresenter(const QString &presenter, QString *error) override
    {
        presenters << presenter;
        if (presenterFails)
            *error = u"murasakid is not running"_s;
        return !presenterFails;
    }
};

} // namespace

class TestPlasmaControl : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_paths.appletsrc = m_dir.filePath(u"plasma-org.kde.plasma.desktop-appletsrc"_s);
        m_paths.stateDir = m_dir.filePath(u"state/murasaki"_s);
        m_paths.dataDirs = {m_dir.filePath(u"data"_s)};
        QDir(m_dir.path()).removeRecursively();
        QDir().mkpath(m_dir.filePath(u"data/plasma/wallpapers/"_s + kPluginId));
        QFile metadata(m_dir.filePath(u"data/plasma/wallpapers/"_s + kPluginId + u"/metadata.json"_s));
        QVERIFY(metadata.open(QIODevice::WriteOnly));
        metadata.write("{}");
        QFile config(m_paths.appletsrc);
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("[Containments][1]\nwallpaperplugin=org.kde.image\n");
    }

    void scriptRunsInAJsEngine();
    void parsesTheQueryOutput();
    void enableRecordsAndSwitches();
    void enableTwiceKeepsTheFirstRecording();
    void enableNeedsThePackage();
    void enableFailsWhenPlasmashellIsDown();
    void enableDetectsASwitchThatDidNothing();
    void enableToleratesAMissingDaemon();
    void dryRunChangesNothing();
    void disableRestoresThePreviousPlugins();
    void disableLeavesForeignChoicesAlone();
    void disableWithoutRecordingUsesTheDefault();
    void statusListsDesktops();
    void recognisesAPlasmaSession();
    void disableRestoresThePreviousPresenter();
    void dryRunSkipsDesktopsAlreadyOurs();
    void failedEnableTellsHowToRestore();

private:
    QTemporaryDir m_dir;
    Paths m_paths;
    Options options(bool dryRun = false) const
    {
        Options o;
        o.dryRun = dryRun;
        o.paths = m_paths;
        return o;
    }
};

void TestPlasmaControl::scriptRunsInAJsEngine()
{
    // The real thing runs inside plasmashell; here only the syntax and the logic are checked against QJSEngine
    // with stand-ins for desktops() and print().
    QJSEngine engine;
    engine.evaluate(uR"(
        var log = [];
        function print(s) { log.push(s); }
        function mk(id, plugin) { return {id: id, wallpaperPlugin: plugin, reloadConfig: function() { log.push("reload " + this.id); }}; }
        var ds = [mk(1, "org.kde.image"), mk(2, "org.kde.slideshow")];
        function desktops() { return ds; }
    )"_s);
    QVERIFY(!engine.evaluate(queryScript()).isError());
    QCOMPARE(engine.evaluate(u"log[0]"_s).toString(),
             u"[{\"id\":\"1\",\"plugin\":\"org.kde.image\"},{\"id\":\"2\",\"plugin\":\"org.kde.slideshow\"}]"_s);
    // Only the named desktops change, including a plugin id that needs escaping.
    const QJSValue result = engine.evaluate(switchScript({{u"2"_s, u"a\"b'c"_s}}));
    QVERIFY2(!result.isError(), qPrintable(result.toString()));
    QCOMPARE(engine.evaluate(u"ds[0].wallpaperPlugin"_s).toString(), u"org.kde.image"_s);
    QCOMPARE(engine.evaluate(u"ds[1].wallpaperPlugin"_s).toString(), u"a\"b'c"_s);
    QCOMPARE(engine.evaluate(u"log[1]"_s).toString(), u"reload 2"_s);
}

void TestPlasmaControl::parsesTheQueryOutput()
{
    QList<Desktop> desktops;
    QVERIFY(parseDesktops(u"[{\"id\":\"1\",\"plugin\":\"a\"},{\"id\":\"7\",\"plugin\":\"b\"}]\n"_s, &desktops));
    QCOMPARE(desktops, (QList<Desktop>{{u"1"_s, u"a"_s}, {u"7"_s, u"b"_s}}));
    QVERIFY(parseDesktops(u"[]"_s, &desktops));
    QVERIFY(desktops.isEmpty());
    QVERIFY(!parseDesktops(u"qml: warning\n[]"_s, &desktops));
    QVERIFY(!parseDesktops(u"{}"_s, &desktops));
    QVERIFY(!parseDesktops(u"[{\"plugin\":\"a\"}]"_s, &desktops));
}

void TestPlasmaControl::enableRecordsAndSwitches()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}, {u"4"_s, u"org.kde.slideshow"_s}};
    const Result result = enable(backend, options());
    QVERIFY2(result.ok, qPrintable(result.error));
    for (const Desktop &d : std::as_const(backend.desktops))
        QCOMPARE(d.plugin, kPluginId);
    QCOMPARE(backend.presenters, QStringList{u"plasma"_s});
    QVERIFY(result.active);

    const QJsonObject state = QJsonDocument::fromJson([&] {
        QFile f(m_paths.stateFile());
        [&] { QVERIFY(f.open(QIODevice::ReadOnly)); }();
        return f.readAll();
    }()).object();
    QCOMPARE(state.value(u"previous"_s).toObject().value(u"1"_s).toString(), u"org.kde.image"_s);
    QCOMPARE(state.value(u"previous"_s).toObject().value(u"4"_s).toString(), u"org.kde.slideshow"_s);
    // The config backup is a byte-for-byte copy.
    QFile backup(m_paths.backupFile());
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QVERIFY(backup.readAll().contains("wallpaperplugin=org.kde.image"));
}

void TestPlasmaControl::enableTwiceKeepsTheFirstRecording()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.slideshow"_s}};
    QVERIFY(enable(backend, options()).ok);
    // The config is rewritten by plasmashell meanwhile; the first backup must survive.
    QFile config(m_paths.appletsrc);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
    config.write("changed");
    config.close();
    const QStringList scriptsAfterFirst = backend.scripts;
    QVERIFY(enable(backend, options()).ok);
    QCOMPARE(backend.scripts.size(), scriptsAfterFirst.size() + 1 - 0); // only the query ran again, no switch
    QVERIFY(disable(backend, options()).ok);
    QCOMPARE(backend.desktops.first().plugin, u"org.kde.slideshow"_s);
    QFile backup(m_paths.backupFile());
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QVERIFY(backup.readAll().contains("wallpaperplugin=org.kde.image"));
}

void TestPlasmaControl::enableNeedsThePackage()
{
    m_paths.dataDirs = {m_dir.filePath(u"nowhere"_s)};
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}};
    const Result result = enable(backend, options());
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(kPluginId));
    QVERIFY(backend.scripts.isEmpty());
    QVERIFY(!QFile::exists(m_paths.stateFile()));
}

void TestPlasmaControl::enableFailsWhenPlasmashellIsDown()
{
    FakeBackend backend;
    backend.plasmaShellDown = true;
    const Result result = enable(backend, options());
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(u"plasmashell"_s));
    QVERIFY(!QFile::exists(m_paths.stateFile()));
    QVERIFY(backend.presenters.isEmpty());

    backend.plasmaShellDown = false;
    backend.garbage = u"not json"_s;
    QVERIFY(!enable(backend, options()).ok);
    backend.garbage = u"[]"_s;
    QVERIFY(!enable(backend, options()).ok); // no containment at all
}

void TestPlasmaControl::enableDetectsASwitchThatDidNothing()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}};
    backend.ignoreSwitch = true;
    const Result result = enable(backend, options());
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(u"did not switch"_s));
    QVERIFY(backend.presenters.isEmpty());
}

void TestPlasmaControl::enableToleratesAMissingDaemon()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}};
    backend.presenterFails = true;
    const Result result = enable(backend, options());
    QVERIFY(result.ok);
    QVERIFY(result.lines.join(u'\n').contains(u"warning"_s));
}

void TestPlasmaControl::dryRunChangesNothing()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}};
    const Result result = enable(backend, options(true));
    QVERIFY(result.ok);
    QVERIFY(result.lines.join(u'\n').contains(u"var map"_s));
    QCOMPARE(backend.desktops.first().plugin, u"org.kde.image"_s);
    QVERIFY(backend.presenters.isEmpty());
    QVERIFY(!QFile::exists(m_paths.stateFile()));
    QVERIFY(!QFile::exists(m_paths.backupFile()));
    QCOMPARE(backend.scripts, QStringList{queryScript()}); // only the read-only query

    QVERIFY(enable(backend, options()).ok);
    backend.scripts.clear();
    backend.presenters.clear();
    const Result off = disable(backend, options(true));
    QVERIFY(off.ok);
    QCOMPARE(backend.desktops.first().plugin, kPluginId);
    QVERIFY(backend.presenters.isEmpty());
    QVERIFY(QFile::exists(m_paths.stateFile()));
}

void TestPlasmaControl::disableRestoresThePreviousPlugins()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}, {u"4"_s, u"org.kde.slideshow"_s}};
    QVERIFY(enable(backend, options()).ok);
    backend.presenters.clear();
    const Result result = disable(backend, options());
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(backend.desktops, (QList<Desktop>{{u"1"_s, u"org.kde.image"_s}, {u"4"_s, u"org.kde.slideshow"_s}}));
    QCOMPARE(backend.presenters, QStringList{u"auto"_s});
    QVERIFY(!QFile::exists(m_paths.stateFile()));
    QVERIFY(!result.active);
}

void TestPlasmaControl::disableLeavesForeignChoicesAlone()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}, {u"4"_s, u"org.kde.image"_s}};
    QVERIFY(enable(backend, options()).ok);
    backend.desktops[1].plugin = u"org.kde.potd"_s; // the user picks something else for desktop 4
    QVERIFY(disable(backend, options()).ok);
    QCOMPARE(backend.desktops[0].plugin, u"org.kde.image"_s);
    QCOMPARE(backend.desktops[1].plugin, u"org.kde.potd"_s);
}

void TestPlasmaControl::disableWithoutRecordingUsesTheDefault()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, kPluginId}, {u"2"_s, u"org.kde.potd"_s}};
    QVERIFY(disable(backend, options()).ok);
    QCOMPARE(backend.desktops[0].plugin, kDefaultPlugin);
    QCOMPARE(backend.desktops[1].plugin, u"org.kde.potd"_s);

    // Nothing of ours anywhere: no switch script at all, but the presenter goes back to auto.
    backend.scripts.clear();
    backend.presenters.clear();
    QVERIFY(disable(backend, options()).ok);
    QCOMPARE(backend.scripts, QStringList{queryScript()});
    QCOMPARE(backend.presenters, QStringList{u"auto"_s});
}

void TestPlasmaControl::statusListsDesktops()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, kPluginId}, {u"2"_s, u"org.kde.image"_s}};
    const Result result = status(backend, options());
    QVERIFY(result.ok);
    QVERIFY(result.active);
    QCOMPARE(result.lines.at(0), u"desktop 1\t%1\tmurasaki"_s.arg(kPluginId));
    QCOMPARE(result.lines.at(1), u"desktop 2\torg.kde.image\tother"_s);
    QVERIFY(result.lines.join(u'\n').contains(u"active: partly"_s));
    QVERIFY(backend.presenters.isEmpty());
}

void TestPlasmaControl::recognisesAPlasmaSession()
{
    const QByteArray saved = qgetenv("XDG_CURRENT_DESKTOP");
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    QVERIFY(isPlasmaSession());
    qputenv("XDG_CURRENT_DESKTOP", "ubuntu:kde");
    QVERIFY(isPlasmaSession());
    qputenv("XDG_CURRENT_DESKTOP", "GNOME");
    QVERIFY(!isPlasmaSession());
    qputenv("XDG_CURRENT_DESKTOP", "");
    QVERIFY(!isPlasmaSession());
    if (saved.isNull())
        qunsetenv("XDG_CURRENT_DESKTOP");
    else
        qputenv("XDG_CURRENT_DESKTOP", saved);
}

void TestPlasmaControl::disableRestoresThePreviousPresenter()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}};
    backend.currentPresenter = u"layer-shell"_s;
    QVERIFY(enable(backend, options()).ok);
    QVERIFY(enable(backend, options()).ok); // the first recording wins, not "plasma"
    backend.currentPresenter = u"plasma"_s;
    backend.presenters.clear();
    QVERIFY(disable(backend, options()).ok);
    QCOMPARE(backend.presenters, QStringList{u"layer-shell"_s});
}

void TestPlasmaControl::dryRunSkipsDesktopsAlreadyOurs()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, kPluginId}};
    Result result = enable(backend, options(true));
    QVERIFY(result.ok);
    QVERIFY(result.lines.join(u'\n').contains(u"nothing to change"_s));
    QVERIFY(!result.lines.join(u'\n').contains(u"var map"_s));
    backend.desktops = {{u"1"_s, kPluginId}, {u"2"_s, u"org.kde.image"_s}};
    result = enable(backend, options(true));
    QVERIFY(!result.lines.join(u'\n').contains(u"desktop 1:"_s));
    QVERIFY(result.lines.join(u'\n').contains(u"desktop 2:"_s));
}

void TestPlasmaControl::failedEnableTellsHowToRestore()
{
    FakeBackend backend;
    backend.desktops = {{u"1"_s, u"org.kde.image"_s}};
    backend.ignoreSwitch = true;
    const Result result = enable(backend, options());
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(u"murasakictl plasma disable"_s));
    QVERIFY(QFile::exists(m_paths.stateFile()));
}

QTEST_GUILESS_MAIN(TestPlasmaControl)
#include "tst_plasmacontrol.moc"
