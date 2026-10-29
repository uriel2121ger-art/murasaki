// The host side of the shortcuts of one wallpaper: the properties the engine is given, the icons it is sent, and the decision about a
// script's request to start an application. The engine is the test, on the other end of a socketpair; nothing is started.
// docs/design/specs/2026-10-21-user-shortcuts-design.md

#include "bridge/channel.h"
#include "bridge/shortcutview.h"
#include "bridgeclient.h"
#include "shortcut/iconrender.h"
#include "shortcut/shortcutitem.h"
#include "testutil.h"

#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki::shortcut;
using murasaki::BridgeClient;
namespace bridge = murasaki::bridge;

namespace {

struct Rig {
    bridge::Channel engine;
    std::unique_ptr<BridgeClient> client;
    explicit Rig(uint32_t version = bridge::kShortcutVersion)
    {
        auto sp = bridge::makeSocketPair();
        if (!sp)
            qFatal("socketpair");
        engine = bridge::Channel(std::move(sp->first));
        engine.setNonblocking(true);
        client = std::make_unique<BridgeClient>(std::move(sp->second));
        engine.send(bridge::msg::Hello{version, 1});
        bridge::msg::Viewport vp{};
        vp.id = 1;
        vp.width = 64;
        vp.height = 32;
        engine.send(vp);
        QSignalSpy ready(client.get(), &BridgeClient::ready);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    }
    struct Icon {
        QString name;
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    // the icons the host sent so far
    QList<Icon> icons()
    {
        QList<Icon> out;
        QTest::qWait(30);
        for (;;) {
            bridge::ReceiveResult r = engine.receive();
            if (r.status != bridge::ChannelStatus::Ok)
                return out;
            if (r.message.type != static_cast<uint32_t>(bridge::kShortcutIconType))
                continue;
            bridge::msg::ShortcutIcon m{};
            if (!bridge::decode(r.message.payload, m) || !bridge::msg::validate(m, r.message.fds.size()))
                qFatal("the host sent an invalid ShortcutIcon");
            Icon icon{QString::fromUtf8(m.name), m.width, m.height, {}};
            if (m.width) {
                auto px = bridge::readSealedPixels(std::move(r.message.fds[0]), m.width, m.height);
                if (!px)
                    qFatal("the host sent an icon that is not a sealed memfd of the right size");
                icon.rgba = std::move(*px);
            }
            out.append(std::move(icon));
        }
    }
    void launch(const char *name)
    {
        bridge::msg::ShortcutLaunch m{};
        m.serial = 1;
        bridge::msg::setShortcutName(m, name);
        engine.send(m);
    }
};

QByteArray png(int w, int h, QColor color)
{
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

struct Fixture {
    QTemporaryDir dir;
    QString item, apps, icons;
    qint64 now = 10000;
    LaunchLimiter limiter;
    Fixture()
    {
        item = dir.filePath(u"item"_s);
        apps = dir.filePath(u"apps/applications"_s);
        icons = dir.filePath(u"icons"_s);
        testutil::writeFile(item + u"/project.json"_s,
                            R"({"general":{"properties":{"launcher1":{"type":"usershortcut","value":""},"launcher2":{"type":"usershortcut","value":""},"other":{"type":"slider","value":1}}}})");
        icons = QFileInfo(icons).absoluteFilePath();
    }
    void app(const QString &id, const QString &name, const QString &icon)
    {
        testutil::writeFile(apps + u"/"_s + id, QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nExec=true\nIcon=%2\n").arg(name, icon).toUtf8());
    }
    ShortcutEnvironment env()
    {
        QDir().mkpath(icons);
        ShortcutEnvironment e;
        e.roots = {apps};
        e.desktop.currentDesktops = {u"KDE"_s};
        e.desktop.hasExecutable = [](const QString &) { return true; };
        e.iconRoots = {QFileInfo(icons).canonicalFilePath()};
        e.themeName = u"mytheme"_s;
        e.themePaths = {icons};
        e.nowMs = [this] { return now; };
        e.useThreads = false;
        return e;
    }
};

// (kept out of the test class: moc does not read raw strings inside slots)
const char kNoShortcuts[] = R"({"general":{"properties":{"a":{"type":"bool"}}}})";
const char kVector[] = R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="8" height="8"/></svg>)";

} // namespace

class TstShortcutHost : public QObject
{
    Q_OBJECT

private slots:
    void init() { ShortcutHub::shared().setLaunchAllowed(true); }

    void theEngineGetsTheNameOfTheApplicationNeverItsId()
    {
        Fixture f;
        f.app(u"a.desktop"_s, u"Alpha"_s, u"x"_s);
        ShortcutItem item(f.item, f.env(), &f.limiter);
        QCOMPARE(item.keys(), (QStringList{u"launcher1"_s, u"launcher2"_s}));
        const QVariantMap out = item.engineProperties({{u"launcher1"_s, u"a.desktop"_s}, {u"launcher2"_s, u"gone.desktop"_s}, {u"other"_s, 3}, {u"unrelated"_s, u"a.desktop"_s}});
        QCOMPARE(out.value(u"launcher1"_s).toString(), u"Alpha"_s);
        QCOMPARE(out.value(u"launcher2"_s).toString(), QString()); // not installed: nothing chosen
        QCOMPARE(out.value(u"other"_s).toInt(), 3);
        QCOMPARE(out.value(u"unrelated"_s).toString(), u"a.desktop"_s); // not a shortcut property: left alone
        for (const QVariant &v : out) // the id is nowhere
            QVERIFY(v.toString() != u"gone.desktop"_s);
        // a shortcut property the GUI did not send is not invented; a non-string stays as it is
        QVERIFY(!item.engineProperties({}).contains(u"launcher1"_s));
        QCOMPARE(item.engineProperties({{u"launcher1"_s, 5}}).value(u"launcher1"_s).toInt(), 5);
    }

    void anItemWithoutShortcutPropertiesHasNoKeys()
    {
        Fixture f;
        testutil::writeFile(f.item + u"/project.json"_s, kNoShortcuts);
        ShortcutItem item(f.item, f.env(), &f.limiter);
        QVERIFY(item.keys().isEmpty());
    }

    void theIconOfTheChosenApplicationIsSentScaledAndCentred()
    {
        Fixture f;
        testutil::writeFile(f.icons + u"/wide.png"_s, png(64, 32, QColor(10, 200, 30)));
        f.app(u"a.desktop"_s, u"Alpha"_s, f.icons + u"/wide.png"_s);
        Rig rig;
        ShortcutItem item(f.item, f.env(), &f.limiter);
        item.attach(rig.client.get());
        QVERIFY(rig.icons().isEmpty()); // nothing is chosen yet: the engine's default stands
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        const auto sent = rig.icons();
        QCOMPARE(sent.size(), qsizetype{1});
        QCOMPARE(sent[0].name, u"launcher1"_s);
        QCOMPARE(sent[0].width, 256u);
        QCOMPARE(sent[0].height, 256u);
        const auto px = [&](int x, int y) { return &sent[0].rgba[(static_cast<size_t>(y) * 256 + static_cast<size_t>(x)) * 4]; };
        QCOMPARE(int(px(128, 128)[0]), 10); // the picture (straight alpha)
        QCOMPARE(int(px(128, 128)[1]), 200);
        QCOMPARE(int(px(128, 128)[3]), 255);
        QCOMPARE(int(px(128, 10)[3]), 0);   // 64 x 32 fits as 256 x 128: transparent above and below
        QCOMPARE(int(px(128, 250)[3]), 0);
    }

    void clearingTheChoiceClearsTheIconAndAnotherChoiceReplacesIt()
    {
        Fixture f;
        testutil::writeFile(f.icons + u"/a.png"_s, png(16, 16, QColor(255, 0, 0)));
        testutil::writeFile(f.icons + u"/b.png"_s, png(16, 16, QColor(0, 0, 255)));
        f.app(u"a.desktop"_s, u"Alpha"_s, f.icons + u"/a.png"_s);
        f.app(u"b.desktop"_s, u"Bravo"_s, f.icons + u"/b.png"_s);
        Rig rig;
        ShortcutItem item(f.item, f.env(), &f.limiter);
        item.attach(rig.client.get());
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        QCOMPARE(rig.icons().size(), qsizetype{1});
        // the slot is limited to one icon a second: the next waits for its turn
        item.assignmentsChanged({{u"launcher1"_s, u"b.desktop"_s}});
        QVERIFY(rig.icons().isEmpty());
        QTest::qWait(1100);
        auto sent = rig.icons();
        QCOMPARE(sent.size(), qsizetype{1});
        QCOMPARE(int(sent[0].rgba[(128 * 256 + 128) * 4 + 2]), 255);
        item.assignmentsChanged({{u"launcher1"_s, QString()}});
        QTest::qWait(1100);
        sent = rig.icons();
        QCOMPARE(sent.size(), qsizetype{1});
        QCOMPARE(sent[0].width, 0u); // cleared
        QVERIFY(sent[0].rgba.empty());
    }

    void anApplicationWhoseIconCannotBeUsedSendsNothing()
    {
        Fixture f;
        QDir().mkpath(f.icons);
        // outside the icon directories, over the size, too wide, an SVG, not a picture, a missing file
        testutil::writeFile(f.dir.filePath(u"outside.png"_s), png(8, 8, Qt::red));
        testutil::writeFile(f.icons + u"/big.png"_s, QByteArray(5 * 1024 * 1024, 'x'));
        testutil::writeFile(f.icons + u"/wide.png"_s, png(5000, 1, Qt::red));
        testutil::writeFile(f.icons + u"/vector.svg"_s, kVector);
        testutil::writeFile(f.icons + u"/text.png"_s, "not a picture");
        const QStringList specs = {f.dir.filePath(u"outside.png"_s), f.icons + u"/big.png"_s, f.icons + u"/wide.png"_s, f.icons + u"/vector.svg"_s, f.icons + u"/text.png"_s,
                                   f.icons + u"/missing.png"_s, f.icons + u"/../outside.png"_s};
        const auto roots = QStringList{QFileInfo(f.icons).canonicalFilePath()};
        for (const QString &spec : specs)
            QVERIFY2(!loadIconFile(spec, roots), qPrintable(spec));
        // only png, jpeg, webp and gif: a BMP or an XPM is no icon
        {
            QImage bmp(8, 8, QImage::Format_RGB32);
            bmp.fill(Qt::red);
            QVERIFY(bmp.save(f.icons + u"/plain.bmp"_s, "BMP"));
            QVERIFY(!loadIconFile(f.icons + u"/plain.bmp"_s, roots));
            QVERIFY(bmp.save(f.icons + u"/plain.xpm"_s, "XPM"));
            QVERIFY(!loadIconFile(f.icons + u"/plain.xpm"_s, roots));
            QVERIFY(bmp.save(f.icons + u"/plain.jpg"_s, "JPG"));
            QVERIFY(loadIconFile(f.icons + u"/plain.jpg"_s, roots));
        }
        // a symlink inside the directory to a file outside it is judged by where it leads
        QFile::link(f.dir.filePath(u"outside.png"_s), f.icons + u"/link.png"_s);
        QVERIFY(!loadIconFile(f.icons + u"/link.png"_s, roots));
        // and a picture that is fine is loaded
        testutil::writeFile(f.icons + u"/ok.png"_s, png(8, 8, Qt::red));
        QVERIFY(loadIconFile(f.icons + u"/ok.png"_s, roots));
        // none of them reaches the engine: the slot keeps its default
        f.app(u"a.desktop"_s, u"Alpha"_s, f.icons + u"/big.png"_s);
        Rig rig;
        ShortcutItem item(f.item, f.env(), &f.limiter);
        item.attach(rig.client.get());
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        QVERIFY(rig.icons().isEmpty());
        QVERIFY(!fitIcon(QImage()));
    }

    void aThemeIconIsFoundByItsFilesThroughInheritanceAndSizes()
    {
        Fixture f;
        QDir().mkpath(f.icons);
        testutil::writeFile(f.icons + u"/mytheme/index.theme"_s, "[Icon Theme]\nName=My\nInherits=base,other\n");
        testutil::writeFile(f.icons + u"/base/index.theme"_s, "[Icon Theme]\nName=Base\n");
        testutil::writeFile(f.icons + u"/base/48x48/apps/inherited.png"_s, png(48, 48, Qt::red));
        testutil::writeFile(f.icons + u"/base/256x256/apps/inherited.png"_s, png(256, 256, Qt::green)); // the bigger one wins
        testutil::writeFile(f.icons + u"/mytheme/apps/64/own.png"_s, png(64, 64, Qt::blue));              // the "apps/<size>" layout
        testutil::writeFile(f.icons + u"/hicolor/32x32/apps/fallback.png"_s, png(32, 32, Qt::yellow));
        testutil::writeFile(QFileInfo(f.icons).absolutePath() + u"/pixmaps/loose.png"_s, png(32, 32, Qt::cyan));
        const QStringList paths{f.icons};
        const auto found = [&](const QString &name) { return findThemeIcon(name, u"mytheme"_s, paths); };
        QVERIFY(found(u"inherited"_s));
        QVERIFY(found(u"inherited"_s)->contains(u"256x256"_s));
        QVERIFY(found(u"own"_s));
        QVERIFY(found(u"fallback"_s));
        QVERIFY(found(u"loose"_s));
        QVERIFY(!found(u"absent"_s));
        // a name is a name: no path, no dots at the start, nothing long
        for (const QString &bad : {u"../inherited"_s, u"a/b"_s, u".hidden"_s, u""_s, QString(300, u'a'), u"x\ny"_s})
            QVERIFY2(!found(bad), qPrintable(bad));
#ifdef MURASAKI_SHORTCUT_SVG
        // a vector icon (Breeze ships only those) is drawn by QtSvg, in whatever thread asks
        testutil::writeFile(f.icons + u"/mytheme/scalable/apps/vector.svg"_s, kVector);
        const auto vector = findThemeIcon(u"vector"_s, u"mytheme"_s, paths);
        QVERIFY(vector);
        const auto drawn = loadThemeIconFile(*vector, {QFileInfo(f.icons).canonicalFilePath()});
        QVERIFY(drawn);
        QCOMPARE(drawn->width(), kIconSide);
        QVERIFY(qAlpha(drawn->pixel(128, 128)) == 255);
        testutil::writeFile(f.icons + u"/mytheme/scalable/apps/broken.svg"_s, "<svg");
        QVERIFY(!loadThemeIconFile(f.icons + u"/mytheme/scalable/apps/broken.svg"_s, {QFileInfo(f.icons).canonicalFilePath()}));
#endif
        // a theme that inherits itself or something silly does not loop
        testutil::writeFile(f.icons + u"/loop/index.theme"_s, "[Icon Theme]\nInherits=loop,../x,.hidden\n");
        QVERIFY(!findThemeIcon(u"absent"_s, u"loop"_s, paths));
        // and the worker path sends it
        f.app(u"a.desktop"_s, u"Alpha"_s, u"own"_s);
        Rig rig;
        ShortcutEnvironment env = f.env();
        env.useThreads = true;
        ShortcutItem item(f.item, env, &f.limiter);
        item.attach(rig.client.get());
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        QList<Rig::Icon> sent;
        QTRY_VERIFY_WITH_TIMEOUT((sent += rig.icons(), !sent.isEmpty()), 5000);
        QCOMPARE(sent.size(), qsizetype{1});
        QCOMPARE(int(sent[0].rgba[(128 * 256 + 128) * 4 + 2]), 255); // the blue one
    }

    void theWorkerThreadDeliversTheIconAndAThemeNameWithoutAThemeSendsNothing()
    {
        Fixture f;
        testutil::writeFile(f.icons + u"/a.png"_s, png(16, 16, QColor(255, 0, 0)));
        f.app(u"a.desktop"_s, u"Alpha"_s, f.icons + u"/a.png"_s);
        f.app(u"t.desktop"_s, u"Themed"_s, u"no-such-icon-in-any-theme"_s);
        Rig rig;
        ShortcutEnvironment env = f.env();
        env.useThreads = true; // as the surface runs it
        ShortcutItem item(f.item, env, &f.limiter);
        item.attach(rig.client.get());
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}, {u"launcher2"_s, u"t.desktop"_s}});
        QList<Rig::Icon> sent;
        QTRY_VERIFY_WITH_TIMEOUT((sent += rig.icons(), !sent.isEmpty()), 5000);
        QCOMPARE(sent.size(), qsizetype{1}); // launcher1 only: the theme has no icon for launcher2, which keeps the scene's default
        QCOMPARE(sent[0].name, u"launcher1"_s);
        QCOMPARE(sent[0].width, 256u);
        // an item that goes away while its icon is being decoded is not touched by the answer
        {
            ShortcutItem gone(f.item, env, &f.limiter);
            gone.attach(rig.client.get());
            gone.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        }
        QTest::qWait(300);
    }

    void aProducerBelowVersionFiveIsSentNothing()
    {
        Fixture f;
        testutil::writeFile(f.icons + u"/a.png"_s, png(16, 16, QColor(255, 0, 0)));
        f.app(u"a.desktop"_s, u"Alpha"_s, f.icons + u"/a.png"_s);
        Rig rig(bridge::kMediaVersion);
        ShortcutItem item(f.item, f.env(), &f.limiter);
        item.attach(rig.client.get());
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        QVERIFY(rig.icons().isEmpty());
    }

    void aRequestStartsNothingWithoutARealClickAndEverythingElseInPlace()
    {
        Fixture f;
        f.app(u"a.desktop"_s, u"Alpha"_s, u"x"_s);
        Rig rig;
        ShortcutItem item(f.item, f.env(), &f.limiter);
        item.attach(rig.client.get());
        item.assignmentsChanged({{u"launcher1"_s, u"a.desktop"_s}});
        QSignalSpy approved(&item, &ShortcutItem::launchApproved);
        QSignalSpy refused(&item, &ShortcutItem::launchRefused);
        // no click yet
        rig.launch("launcher1");
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(approved.count(), 0);
        QCOMPARE(refused.takeFirst().at(1).toString(), u"no recent click"_s);
        // a click, then the request
        item.noteClick();
        f.now += 100;
        rig.launch("launcher1");
        QTRY_COMPARE(approved.count(), 1);
        QCOMPARE(approved.takeFirst(), (QVariantList{f.item, u"launcher1"_s}));
        // the click was used: the same one does not start a second
        f.now += 3000;
        rig.launch("launcher1");
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(approved.count(), 0);
        refused.clear();
        // a key that is not a shortcut of the item, and one with nothing chosen
        item.noteClick();
        rig.launch("other");
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(refused.takeFirst().at(1).toString(), u"not a shortcut property of this wallpaper"_s);
        item.noteClick();
        rig.launch("launcher2");
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(refused.takeFirst().at(1).toString(), u"no application assigned"_s);
        QCOMPARE(approved.count(), 0);
        // the user's switch off, or the wallpaper hidden
        ShortcutHub::shared().setLaunchAllowed(false);
        item.noteClick();
        f.now += 3000;
        rig.launch("launcher1");
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(refused.takeFirst().at(1).toString(), u"the setting is off"_s);
        ShortcutHub::shared().setLaunchAllowed(true);
        bool visible = false;
        item.setVisibleProbe([&] { return visible; });
        item.noteClick();
        rig.launch("launcher1");
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(refused.takeFirst().at(1).toString(), u"the wallpaper is not shown"_s);
        QCOMPARE(approved.count(), 0);
        visible = true;
        f.now += 3000;
        item.noteClick(); // (a press older than 0.75 s counts for nothing)
        rig.launch("launcher1");
        QTRY_COMPARE(approved.count(), 1);
    }

    void anUnknownSwitchCountsAsOff()
    {
        // (a process that has not heard from the daemon yet: the hub starts unknown; a fresh item with no setting says no)
        QVERIFY(!murasaki::shortcut::ShortcutHub().launchAllowed());
    }
};

QTEST_MAIN(TstShortcutHost)
#include "tst_shortcuthost.moc"
