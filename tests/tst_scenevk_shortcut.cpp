// murasaki-scene-vk user shortcuts: engine.openUserShortcut(name) asks the host (through a handler) only for a property that holds an
// application, the shortcut slots of a scene are read, and the messages of bridge/shortcutview.h are validated.
// docs/design/specs/2026-10-21-user-shortcuts-design.md

#include "assets/vfs.h"
#include "bridge/shortcutview.h"
#include "render/scenescripts.h"
#include "scene/material.h"
#include "scene/scene.h"
#include "scenevkfixtures.h"
#include "script/scripthost.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cstring>

using namespace murasaki::scene;

namespace {

struct World {
    scenevkfix::Item item;
    std::optional<Scene> scene;
    Vfs vfs{Vfs::forItem("/nonexistent", {})};
    PropertyStore props;
    ScriptFlags flags;
    std::unique_ptr<SceneScripts> scripts;
    std::vector<std::string> asked;
    bool answer = true;

    bool build(const QString &call, const char *properties)
    {
        using namespace scenevkfix;
        item.tex(QStringLiteral("a"), simpleRgba(8, 8));
        item.image(QStringLiteral("a"));
        item.tex(QStringLiteral("out"), simpleRgba(8, 8));
        const QString source = QStringLiteral("let n = 0; export function update(v) { n++; if (n === 1) { thisScene.getLayer('out').text = String(%1); } return v; }").arg(call);
        const QJsonObject driver{{"id", 7}, {"name", "driver"}, {"visible", QJsonObject{{"script", source}, {"value", true}}}};
        const QByteArray objects = R"({"id":1,"name":"a","image":"models/a.json","origin":"0 0 0","size":"4 4"},)"
                                   R"({"id":4,"name":"out","text":"","pointsize":20,"font":"fonts/x.ttf","origin":"10 10 0"},)"
            + QJsonDocument(driver).toJson(QJsonDocument::Compact);
        item.file(QStringLiteral("scene.json"), sceneJson(objects));
        vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        scene = loadScene(vfs, "scene.json", &err);
        if (!scene) {
            return false;
        }
        props.setFromJson(properties);
        scripts = std::make_unique<SceneScripts>(*scene, vfs, props, flags);
        scripts->setShortcutLaunchHandler([this](const std::string &name) {
            asked.push_back(name);
            return answer;
        });
        scripts->start();
        return true;
    }

    QString run()
    {
        ScriptFrame f;
        f.canvasWidth = 1920;
        f.canvasHeight = 1080;
        scripts->update(f);
        for (const ImageLayer &l : scene->layers) {
            if (l.text && scene->nodes[l.node].name == "out") {
                return QString::fromStdString(l.text->value);
            }
        }
        return {};
    }
};

QString ask(const QString &call, const char *properties, std::vector<std::string> *asked = nullptr, bool answer = true)
{
    World w;
    w.answer = answer;
    if (!w.build(call, properties)) {
        return QStringLiteral("(no scene)");
    }
    const QString out = w.run();
    if (asked) {
        *asked = w.asked;
    }
    return out;
}

} // namespace

class TstSceneVkShortcut : public QObject
{
    Q_OBJECT

private slots:
    void aScriptAsksForAPropertyThatHoldsAnApplication()
    {
        std::vector<std::string> asked;
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('launcher1')"), R"({"launcher1":"Firefox"})", &asked), QStringLiteral("true"));
        QCOMPARE(asked, (std::vector<std::string>{"launcher1"}));
    }

    void nothingIsAskedForAnEmptyUnknownOrMalformedProperty()
    {
        std::vector<std::string> asked;
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('launcher1')"), R"({"launcher1":""})", &asked), QStringLiteral("false")); // nothing chosen
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('missing')"), R"({"launcher1":"x"})", &asked), QStringLiteral("false"));  // not a property
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('')"), R"({"": "x"})", &asked), QStringLiteral("false"));
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut(5)"), R"({"5":"x"})", &asked), QStringLiteral("false"));
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut(null)"), R"({})", &asked), QStringLiteral("false"));
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('launcher1')"), R"({"launcher1":3})", &asked), QStringLiteral("false"));   // not an application
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('a\\nb')"), R"({"a\nb":"x"})", &asked), QStringLiteral("false"));          // a control character
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('%1')").arg(QString(64, QLatin1Char('k'))), QByteArray(R"({")" + QByteArray(64, 'k') + R"(":"x"})").constData(), &asked),
                 QStringLiteral("false")); // 64 bytes: too long
        QVERIFY(asked.empty());
    }

    void theAnswerIsWhatTheHandlerSays()
    {
        std::vector<std::string> asked;
        QCOMPARE(ask(QStringLiteral("engine.openUserShortcut('launcher1')"), R"({"launcher1":"Firefox"})", &asked, false), QStringLiteral("false"));
        QCOMPARE(asked.size(), size_t{1}); // it was asked and could not send
    }

    void withoutAHostThereIsNoAction()
    {
        World w;
        QVERIFY(w.build(QStringLiteral("engine.openUserShortcut('launcher1')"), R"({"launcher1":"Firefox"})"));
        w.scripts->setShortcutLaunchHandler({});
        QCOMPARE(w.run(), QStringLiteral("false"));
    }

    void aUsershortcutTextureSlotIsABindingNotADegrade()
    {
        const auto pass = parseJson(R"({"usertextures":[null,{"name":"s01c","type":"usershortcut"},{"name":"$mediaThumbnail","type":"system"},{"name":"","type":"usershortcut"}]})");
        QVERIFY(pass);
        const auto bindings = mediaBindings(*pass);
        QCOMPARE(bindings.size(), size_t{2});
        QCOMPARE(bindings[0].slot, 1u);
        QCOMPARE(bindings[0].shortcut, std::string("s01c"));
        QCOMPARE(bindings[1].slot, 2u);
        QVERIFY(bindings[1].shortcut.empty());
        QVERIFY(userBindings(*pass).empty()); // nothing is lost: the host fills the slot
    }

    // ---- the wire messages ----
    void shortcutNamesAreWellFormedUtf8WithoutControls()
    {
        using namespace murasaki::bridge;
        const auto make = [](const char *text, size_t n) {
            msg::ShortcutLaunch m{};
            std::memcpy(m.name, text, n);
            return m;
        };
        QVERIFY(msg::validate(make("launcher1", 9), 0));
        QVERIFY(msg::validate(make("\xe8\xba\xab\xe4\xbd\x93", 6), 0)); // 身体
        QVERIFY(!msg::validate(make("", 0), 0));                          // empty
        QVERIFY(!msg::validate(make("a\x01" "b", 3), 0));                 // a control character
        QVERIFY(!msg::validate(make("\xff", 1), 0));                      // not UTF-8
        QVERIFY(!msg::validate(make("\xc0\xaf", 2), 0));                  // overlong
        QVERIFY(!msg::validate(make("\xed\xa0\x80", 3), 0));              // a surrogate
        QVERIFY(!msg::validate(make("\xe8\xba", 2), 0));                  // cut short
        QVERIFY(!msg::validate(make("a", 1), 1));                         // a launch carries no fd
        msg::ShortcutLaunch full{};
        std::memset(full.name, 'k', sizeof full.name); // no NUL in the field
        QVERIFY(!msg::validate(full, 0));
        msg::ShortcutLaunch dirty = make("abc", 3);
        dirty.name[10] = 'x'; // data after the NUL
        QVERIFY(!msg::validate(dirty, 0));
        msg::ShortcutLaunch reserved = make("abc", 3);
        reserved.reserved = 1;
        QVERIFY(!msg::validate(reserved, 0));
    }

    void anIconMessageIsValidatedByItsFds()
    {
        using namespace murasaki::bridge;
        const auto icon = [](uint32_t w, uint32_t h, const char *name = "s01c") {
            msg::ShortcutIcon m{w, h, 1, 0, {}};
            msg::setShortcutName(m, name);
            return m;
        };
        QVERIFY(msg::validate(icon(0, 0), 0));
        QVERIFY(!msg::validate(icon(0, 0), 1));
        QVERIFY(msg::validate(icon(256, 256), 1));
        QVERIFY(!msg::validate(icon(256, 256), 0));
        QVERIFY(!msg::validate(icon(256, 256), 2));
        QVERIFY(!msg::validate(icon(257, 4), 1));
        QVERIFY(!msg::validate(icon(4, 0), 1));
        QVERIFY(!msg::validate(icon(0, 4), 1));
        QVERIFY(!msg::validate(icon(16, 16, ""), 1));
        msg::ShortcutIcon reserved = icon(16, 16);
        reserved.reserved = 1;
        QVERIFY(!msg::validate(reserved, 1));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkShortcut)
#include "tst_scenevk_shortcut.moc"
