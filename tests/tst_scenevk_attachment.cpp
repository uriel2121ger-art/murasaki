// murasaki-scene-vk attachments: an image object whose `attachment` names a point of its parent puppet's rig (an MDAT0001 entry
// of the model) stands on that point: it follows the bone's animated matrix. The model is written by the test (puppetfixtures.h).

#include "assets/vfs.h"
#include "puppet/mdl.h"
#include "puppetfixtures.h"
#include "render/puppetdraw.h"
#include "scene/scene.h"
#include "render/scenescripts.h"
#include "scenevkfixtures.h"
#include "script/scripthost.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace murasaki::scene;

namespace {

using namespace puppetfix;

constexpr float kQuarterTurn = 1.57079633f;

// root, an arm 10 units along x that turns a quarter turn about z over one frame, and two points on the arm
std::vector<uint8_t> armModel(int mdatEndError = 0)
{
    return makeMdl({{"root", -1, 0, 0}, {"arm", 0, 10, 0}},
                   {{7, "turn", "loop", 30.0f, 2, {{{frame(0, 0), frame(0, 0), frame(0, 0)}}, {{frame(10, 0, 0.0f), frame(10, 0, kQuarterTurn), frame(10, 0, kQuarterTurn)}}}}},
                   false, "MDLV0023", "MDLS0004", "MDLA0006", false, false, 0.0f,
                   {{"tip", 1, 5, 0}, {"身体：头", 0, 0, 20}, {"turned", 1, 0, 0, kQuarterTurn}}, mdatEndError);
}

struct World {
    scenevkfix::Item item;
    std::optional<Scene> scene;
    std::string error;

    // the puppet "pup" at (500, 500) and one child per entry of `children`: {attachment, origin}; `parentIsPuppet` false makes the
    // children's parent a plain image
    bool build(const std::vector<std::pair<std::string, std::string>> &children, bool parentIsPuppet = true, int mdatEndError = 0, bool groups = false)
    {
        using namespace scenevkfix;
        item.tex(QStringLiteral("red"), simpleRgba(8, 8));
        item.tex(QStringLiteral("a"), simpleRgba(8, 8));
        item.image(QStringLiteral("a"));
        const auto mdl = armModel(mdatEndError);
        item.file(QStringLiteral("models/pup.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(QStringLiteral("models/pup.json"), R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
        item.file(QStringLiteral("materials/pup.json"), R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");
        QByteArray objects = parentIsPuppet ? R"({"id":5,"name":"pup","image":"models/pup.json","origin":"500 500 0","size":"40 40","animationlayers":[{"animation":7,"rate":0}]})"
                                            : R"({"id":5,"name":"pup","image":"models/a.json","origin":"500 500 0","size":"40 40"})";
        int id = 10;
        for (const auto &[attachment, origin] : children) {
            objects += ",{\"id\":" + QByteArray::number(id) + ",\"name\":\"child" + QByteArray::number(id) + "\",\"" + (groups ? "x" : "image") + "\":\"models/a.json\",\"parent\":5,\"attachment\":\""
                + QByteArray::fromStdString(attachment) + "\",\"origin\":\"" + QByteArray::fromStdString(origin) + "\",\"size\":\"4 4\"}";
            ++id;
        }
        item.file(QStringLiteral("scene.json"), sceneJson(objects));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        scene = loadScene(vfs, "scene.json", &error);
        return scene.has_value();
    }

    const ImageLayer &puppetLayer() const
    {
        for (const ImageLayer &l : scene->layers) {
            if (scene->nodes[l.node].name == "pup") {
                return l;
            }
        }
        return scene->layers.front();
    }
    size_t nodeNamed(const char *name) const
    {
        for (size_t i = 0; i < scene->nodes.size(); ++i) {
            if (scene->nodes[i].name == name) {
                return i;
            }
        }
        return 0;
    }
    // moves the puppet's attachment points to `time`, as the renderer does each frame
    void poseAt(double time)
    {
        const ImageLayer &layer = puppetLayer();
        puppetAttachmentPoses(*layer.puppet, {{7, 1.0f, 1.0f}}, time, layer.attachmentPoses->matrices);
    }
    bool degraded(const std::string &reason) const { return std::find(scene->degraded.begin(), scene->degraded.end(), reason) != scene->degraded.end(); }
};

} // namespace

class TstSceneVkAttachment : public QObject
{
    Q_OBJECT

private slots:
    void theReaderKeepsTheAttachmentPoints()
    {
        std::string error;
        const auto puppet = parsePuppet(armModel(), &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QCOMPARE(puppet->attachments.size(), size_t{3});
        QCOMPARE(puppet->attachments[0].name, std::string("tip"));
        QCOMPARE(puppet->attachments[0].bone, 1u);
        QCOMPARE(puppet->attachments[0].local.m[12], 5.0f);
        QCOMPARE(puppet->attachments[1].name, std::string("身体：头")); // UTF-8 as it stands
        QCOMPARE(puppet->attachments[1].local.m[13], 20.0f);
        QVERIFY(std::fabs(puppet->attachments[2].local.m[1] - std::sin(kQuarterTurn)) < 1e-5f); // the rows are transposed into columns
    }

    void aSectionThatDoesNotLineUpIsNotUsedAndTheModelStillReads()
    {
        std::string error;
        const auto puppet = parsePuppet(armModel(3), &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QVERIFY(puppet->attachments.empty());
        QCOMPARE(puppet->bones.size(), size_t{2});
        QCOMPARE(puppet->animations.size(), size_t{1});
    }

    void aChildStandsOnItsPointAndFollowsTheBone()
    {
        World w;
        QVERIFY2(w.build({{"tip", "1 0 0"}}), w.error.c_str());
        QVERIFY(w.scene->degraded.empty());
        const size_t child = w.nodeNamed("child10");
        w.poseAt(0.0); // the arm lies along x: its point is at (15, 0) in the mesh, the child's origin one more
        Mat4 m = nodeWorldMatrix(*w.scene, child);
        QVERIFY(std::fabs(m.at(0, 3) - 516.0f) < 1e-3f && std::fabs(m.at(1, 3) - 500.0f) < 1e-3f);
        w.poseAt(1.0 / 30.0); // a quarter turn: the point is at (10, 5), the origin turned to (0, 1)
        m = nodeWorldMatrix(*w.scene, child);
        QVERIFY(std::fabs(m.at(0, 3) - 510.0f) < 1e-3f);
        QVERIFY(std::fabs(m.at(1, 3) - 506.0f) < 1e-3f);
        // and the child turns with the bone
        QVERIFY(std::fabs(m.at(1, 0) - 1.0f) < 1e-4f && std::fabs(m.at(0, 0)) < 1e-4f);
    }

    void aPointsOwnRotationIsApplied()
    {
        World w;
        QVERIFY2(w.build({{"turned", "2 0 0"}}), w.error.c_str());
        w.poseAt(0.0); // the arm at rest, the point turned a quarter: the origin (2, 0) goes to (0, 2)
        const Mat4 m = nodeWorldMatrix(*w.scene, w.nodeNamed("child10"));
        QVERIFY(std::fabs(m.at(0, 3) - 510.0f) < 1e-3f);
        QVERIFY(std::fabs(m.at(1, 3) - 502.0f) < 1e-3f);
    }

    void aGroupWithNoImageStandsOnItsPointToo()
    {
        World w;
        QVERIFY2(w.build({{"tip", "0 0 0"}}, true, 0, true), w.error.c_str());
        QVERIFY(w.scene->degraded.empty());
        w.poseAt(1.0 / 30.0);
        const Mat4 m = nodeWorldMatrix(*w.scene, w.nodeNamed("child10"));
        QVERIFY(std::fabs(m.at(0, 3) - 510.0f) < 1e-3f && std::fabs(m.at(1, 3) - 505.0f) < 1e-3f);
    }

    void aPointWithAMatrixThatIsNotANumberIsLeftOut()
    {
        std::string error;
        const float nan = std::nanf("");
        const auto mdl = makeMdl({{"root", -1, 0, 0}, {"arm", 0, 10, 0}},
                                 {{7, "turn", "loop", 30.0f, 2, {{{frame(0, 0), frame(0, 0)}}, {{frame(10, 0), frame(10, 0)}}}}}, false, "MDLV0023", "MDLS0004",
                                  "MDLA0006", false, false, 0.0f, {{"bad", 1, nan, 0}, {"good", 1, 5, 0}});
        const auto puppet = parsePuppet(mdl, &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QCOMPARE(puppet->attachments.size(), size_t{1});
        QCOMPARE(puppet->attachments[0].name, std::string("good"));
        QCOMPARE(puppet->animations.size(), size_t{1}); // the section still lines up
    }

    void aFakeAnimationTagInsideTheSectionDoesNotHideTheRealOne()
    {
        std::string error;
        // the section's `end` is wrong, so the animations are looked for by their tag; a point's name that looks like one is no tag
        const auto mdl = makeMdl({{"root", -1, 0, 0}, {"arm", 0, 10, 0}},
                                 {{7, "turn", "loop", 30.0f, 2, {{{frame(0, 0), frame(0, 0)}}, {{frame(10, 0), frame(10, 0)}}}}}, false, "MDLV0023", "MDLS0004",
                                  "MDLA0006", false, false, 0.0f, {{"MDLA0009", 1, 1, 0}, {std::string("MDLA9") + std::string(5, 'x'), 0, 2, 0}}, 3);
        const auto puppet = parsePuppet(mdl, &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QCOMPARE(puppet->bones.size(), size_t{2});
        QCOMPARE(puppet->animations.size(), size_t{1});
        QVERIFY(puppet->attachments.empty()); // the section did not line up
    }

    void aLoopOfParentsIsReported()
    {
        using namespace scenevkfix;
        Item item;
        item.tex(QStringLiteral("a"), simpleRgba(8, 8));
        item.image(QStringLiteral("a"));
        const QByteArray objects = R"({"id":1,"name":"one","image":"models/a.json","parent":2,"size":"4 4"},)"
                                   R"({"id":2,"name":"two","image":"models/a.json","parent":1,"size":"4 4"},)"
                                   R"({"id":3,"name":"self","image":"models/a.json","parent":3,"size":"4 4"},)"
                                   R"({"id":4,"name":"fine","image":"models/a.json","size":"4 4"})";
        item.file(QStringLiteral("scene.json"), sceneJson(objects));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        const auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        const auto count = std::count_if(scene->degraded.begin(), scene->degraded.end(), [](const std::string &d) { return d.rfind("parent loop at object", 0) == 0; });
        QCOMPARE(count, 2); // the pair once, the object under itself once, the plain one not
    }

    void aScriptThatReparentsDropsThePoint()
    {
        using namespace scenevkfix;
        World w;
        QVERIFY2(w.build({{"tip", "1 0 0"}}), w.error.c_str());
        const size_t child = w.nodeNamed("child10");
        QVERIFY(w.scene->nodes[child].attachPoses);
        QCOMPARE(w.scene->nodes[child].attachPoses->users, size_t{1});
        // a script moves the child under nothing, keeping its world place
        const QString source = QStringLiteral("export function update(v) { thisScene.getLayer('child10').setParent(null, true); return v; }");
        const QJsonObject driver{{"id", 7}, {"name", "driver"}, {"visible", QJsonObject{{"script", source}, {"value", true}}}};
        QByteArray objects = R"({"id":5,"name":"pup","image":"models/pup.json","origin":"500 500 0","size":"40 40","animationlayers":[{"animation":7,"rate":0}]},)"
                             R"({"id":10,"name":"child10","image":"models/a.json","parent":5,"attachment":"tip","origin":"1 0 0","size":"4 4"},)"
            + QJsonDocument(driver).toJson(QJsonDocument::Compact);
        w.item.file(QStringLiteral("scene.json"), sceneJson(objects));
        Vfs vfs = Vfs::forItem(w.item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        size_t node = 0;
        for (size_t i = 0; i < scene->nodes.size(); ++i) {
            if (scene->nodes[i].name == "child10") {
                node = i;
            }
        }
        QVERIFY(scene->nodes[node].attachPoses);
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        ScriptFrame f;
        f.canvasWidth = 1920;
        f.canvasHeight = 1080;
        scripts.update(f);
        QVERIFY(!scene->nodes[node].attachPoses);
        QCOMPARE(scene->nodes[node].parentId, -1);
        // its world place is kept: the point (15, 0) and its origin on the puppet at (500, 500)
        const Mat4 m = nodeWorldMatrix(*scene, node);
        QVERIFY(std::fabs(m.at(0, 3) - 516.0f) < 1e-3f && std::fabs(m.at(1, 3) - 500.0f) < 1e-3f);
    }

    void aUnicodeNameIsFound()
    {
        World w;
        QVERIFY2(w.build({{"身体：头", "0 0 0"}}), w.error.c_str());
        QVERIFY(w.scene->degraded.empty());
        w.poseAt(0.0);
        const Mat4 m = nodeWorldMatrix(*w.scene, w.nodeNamed("child10"));
        QVERIFY(std::fabs(m.at(0, 3) - 500.0f) < 1e-3f && std::fabs(m.at(1, 3) - 520.0f) < 1e-3f);
    }

    void aNameThatIsNotThereIsReportedAndTheChildStaysPlain()
    {
        World w;
        QVERIFY2(w.build({{"nowhere", "3 4 0"}}), w.error.c_str());
        QVERIFY(w.degraded("attachment 'nowhere': the parent's model has no such point"));
        const Mat4 m = nodeWorldMatrix(*w.scene, w.nodeNamed("child10"));
        QVERIFY(std::fabs(m.at(0, 3) - 503.0f) < 1e-3f && std::fabs(m.at(1, 3) - 504.0f) < 1e-3f); // relative to the parent's origin
    }

    void aParentThatIsNotAPuppetIsReported()
    {
        World w;
        QVERIFY2(w.build({{"tip", "0 0 0"}}, false), w.error.c_str());
        QVERIFY(w.degraded("attachment 'tip': the parent is not a puppet"));
    }

    void aModelWithoutPointsIsReported()
    {
        World w;
        QVERIFY2(w.build({{"tip", "0 0 0"}}, true, 3), w.error.c_str()); // the section is unusable: no points
        QVERIFY(w.degraded("attachment 'tip': the parent's model has no attachment points"));
    }
};

QTEST_APPLESS_MAIN(TstSceneVkAttachment)
#include "tst_scenevk_attachment.moc"
