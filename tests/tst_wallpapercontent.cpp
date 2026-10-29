#include "testutil.h"
#include "wallpapercontent.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestWallpaperContent : public QObject
{
    Q_OBJECT

private slots:
    void noAssignmentIsBlank();
    void picksDelegateByKindAndStatus();
    void missingItemIsBlackButKeyed();
    void keyIgnoresMuteOnly();
    void keyIgnoresPropertyOverrides();
    void exposesResolvedUserProperties();
    void diffsChangedUserProperties();
    void buildsEntryUrls();
    void entryUrlRejectsBadInput();
    void entryUrlKeepsSpecialCharacters();
    void emptyEntryFileGivesPreview();
    void webItemsListReadableRoots();
    void sceneItemsCarryEngineInputs();
    void sceneKeyIgnoresPropertiesNotAssets();
    void scenePropertyLayering();
    void spanIsPartOfTheKey();
    void networkPermissionIsDeniedByDefaultAndPartOfTheKey();

private:
    static QJsonObject reply(const char *kind, const char *status, const char *fill = "fill",
                             const char *properties = "{}");
};

QJsonObject TestWallpaperContent::reply(const char *kind, const char *status, const char *fill,
                                        const char *properties)
{
    const QString text = QString::fromLatin1(
        "{\"assignment\": {\"output\": \"*\", \"desktop\": \"*\", \"itemId\": \"we:1\", \"muted\": false,"
        " \"fill\": \"%3\", \"properties\": %4},"
        " \"item\": {\"id\": \"we:1\", \"kind\": \"%1\", \"status\": \"%2\", \"contentDir\": \"/w/1\","
        " \"entryFile\": \"/w/1/index.html\", \"preview\": \"/w/1/preview.gif\", \"presetValues\": {},"
        " \"properties\": [{\"key\": \"speed\", \"type\": \"slider\", \"value\": 3}]}}")
                             .arg(QLatin1String(kind), QLatin1String(status), QLatin1String(fill),
                                  QLatin1String(properties));
    return testutil::json(text.toUtf8().constData());
}

void TestWallpaperContent::noAssignmentIsBlank()
{
    const WallpaperContent content =
        WallpaperContent::fromResolveReply(testutil::json("{\"assignment\": null, \"item\": null}"));
    QVERIFY(content.key.isEmpty());
    QCOMPARE(content.kind, u"none"_s);
}

void TestWallpaperContent::picksDelegateByKindAndStatus()
{
    QCOMPARE(WallpaperContent::fromResolveReply(reply("video", "ok")).kind, u"video"_s);
    const WallpaperContent web = WallpaperContent::fromResolveReply(reply("web", "ok"));
    QCOMPARE(web.kind, u"web"_s);
    QVERIFY(web.script.contains(u"\\\"speed\\\":{\\\"value\\\":3}"_s));
    QCOMPARE(web.entryFile, u"/w/1/index.html"_s);
    QCOMPARE(web.contentDir, u"/w/1"_s);
    QCOMPARE(web.previewPath, u"/w/1/preview.gif"_s);
    const WallpaperContent noAssets = WallpaperContent::fromResolveReply(reply("scene", "ok"));
    QCOMPARE(noAssets.kind, u"preview"_s);
    QVERIFY(!noAssets.reason.isEmpty());
    QCOMPARE(WallpaperContent::fromResolveReply(reply("video", "invalid")).kind, u"preview"_s);
    QVERIFY(WallpaperContent::fromResolveReply(reply("video", "ok")).script.isEmpty());
}

void TestWallpaperContent::missingItemIsBlackButKeyed()
{
    QJsonObject r = reply("video", "ok");
    r.insert(u"item"_s, QJsonValue::Null);
    const WallpaperContent content = WallpaperContent::fromResolveReply(r);
    QCOMPARE(content.kind, u"none"_s);
    QCOMPARE(content.itemId, u"we:1"_s);
    QVERIFY(!content.key.isEmpty());
}

void TestWallpaperContent::keyIgnoresMuteOnly()
{
    const QString base = WallpaperContent::fromResolveReply(reply("video", "ok")).key;
    QJsonObject muted = reply("video", "ok");
    QJsonObject assignment = muted.value(u"assignment"_s).toObject();
    assignment.insert(u"muted"_s, true);
    muted.insert(u"assignment"_s, assignment);
    const WallpaperContent mutedContent = WallpaperContent::fromResolveReply(muted);
    QCOMPARE(mutedContent.key, base);
    QVERIFY(mutedContent.muted);

    QVERIFY(WallpaperContent::fromResolveReply(reply("video", "ok", "fit")).key != base);
    QVERIFY(WallpaperContent::fromResolveReply(reply("web", "ok")).key != base);
}

void TestWallpaperContent::keyIgnoresPropertyOverrides()
{
    for (const char *kind : {"web", "video"}) {
        const QString base = WallpaperContent::fromResolveReply(reply(kind, "ok")).key;
        QCOMPARE(WallpaperContent::fromResolveReply(reply(kind, "ok", "fill", "{\"speed\": 9}")).key, base);
        QVERIFY(WallpaperContent::fromResolveReply(reply(kind, "ok", "fit")).key != base);
    }
    QVERIFY(WallpaperContent::fromResolveReply(reply("web", "ok")).key
            != WallpaperContent::fromResolveReply(reply("video", "ok")).key);
    // Other non-property options still reload.
    QJsonObject r = reply("web", "ok");
    const QString base = WallpaperContent::fromResolveReply(r).key;
    QJsonObject assignment = r.value(u"assignment"_s).toObject();
    assignment.insert(u"itemId"_s, u"we:2"_s);
    r.insert(u"assignment"_s, assignment);
    QVERIFY(WallpaperContent::fromResolveReply(r).key != base);
}

void TestWallpaperContent::exposesResolvedUserProperties()
{
    const WallpaperContent plain = WallpaperContent::fromResolveReply(reply("web", "ok"));
    QCOMPARE(plain.userProperties.value(u"speed"_s).toObject().value(u"value"_s).toInt(), 3);
    const WallpaperContent over = WallpaperContent::fromResolveReply(reply("web", "ok", "fill", "{\"speed\": 9}"));
    QCOMPARE(over.userProperties.value(u"speed"_s).toObject().value(u"value"_s).toInt(), 9);
    QVERIFY(WallpaperContent::fromResolveReply(reply("video", "ok")).userProperties.isEmpty());
}

void TestWallpaperContent::diffsChangedUserProperties()
{
    const QJsonObject before = testutil::json("{\"a\": {\"value\": 1}, \"b\": {\"value\": \"x\"}, \"c\": {\"value\": true}}");
    QVERIFY(changedUserProperties(before, before).isEmpty());
    const QJsonObject after = testutil::json("{\"a\": {\"value\": 2}, \"b\": {\"value\": \"x\"}, \"d\": {\"value\": 0}}");
    const QJsonObject changed = changedUserProperties(before, after);
    QCOMPARE(changed.keys(), (QStringList{u"a"_s, u"d"_s})); // c vanished: nothing to tell the page
    QCOMPARE(changed.value(u"a"_s).toObject().value(u"value"_s).toInt(), 2);
}

void TestWallpaperContent::buildsEntryUrls()
{
    const QUrl url = itemEntryUrl(u"abc123"_s, u"/w/1"_s, u"/w/1/Arthesian Library/index.html"_s);
    QVERIFY(url.isValid());
    QCOMPARE(url.scheme(), u"murasaki-item"_s);
    QCOMPARE(url.host(), u"abc123"_s);
    QCOMPARE(url.path(QUrl::FullyDecoded), u"/Arthesian Library/index.html"_s);
    QVERIFY(url.toString(QUrl::FullyEncoded).contains(u"Arthesian%20Library"_s));
    QVERIFY(!itemEntryUrl(u"abc123"_s, u"/w/1"_s, u"/w/2/index.html"_s).isValid());
    QVERIFY(!itemEntryUrl(QString(), u"/w/1"_s, u"/w/1/index.html"_s).isValid());
}

void TestWallpaperContent::entryUrlRejectsBadInput()
{
    QVERIFY(!itemEntryUrl(u"abc123"_s, u"/w/1"_s, u"/w/1/a/../../x"_s).isValid());
    QVERIFY(!itemEntryUrl(u"abc123"_s, u"/w/1"_s, u"/w/1"_s).isValid()); // relative path is "."
    QVERIFY(!itemEntryUrl(u"abc123"_s, QString(), u"/w/1/index.html"_s).isValid());
    QVERIFY(!itemEntryUrl(u"abc123"_s, u"/w/1"_s, QString()).isValid());
    QVERIFY(!itemEntryUrl(u"Abc123"_s, u"/w/1"_s, u"/w/1/index.html"_s).isValid());
    QVERIFY(!itemEntryUrl(u"abc_123"_s, u"/w/1"_s, u"/w/1/index.html"_s).isValid());
}

void TestWallpaperContent::entryUrlKeepsSpecialCharacters()
{
    for (const QString &name : {u"a#b.html"_s, u"what?.html"_s, u"100%.html"_s, u"50%25.html"_s}) {
        const QUrl url = itemEntryUrl(u"abc123"_s, u"/w/1"_s, u"/w/1/sub dir/"_s + name);
        QVERIFY(url.isValid());
        QVERIFY(!url.hasFragment());
        QVERIFY(!url.hasQuery());
        QCOMPARE(url.path(QUrl::FullyDecoded), u"/sub dir/"_s + name);
    }
}

void TestWallpaperContent::emptyEntryFileGivesPreview()
{
    QJsonObject r = reply("video", "ok");
    QJsonObject item = r.value(u"item"_s).toObject();
    item.insert(u"entryFile"_s, QString());
    r.insert(u"item"_s, item);
    QCOMPARE(WallpaperContent::fromResolveReply(r).kind, u"preview"_s);
    item.insert(u"kind"_s, u"web"_s);
    r.insert(u"item"_s, item);
    QCOMPARE(WallpaperContent::fromResolveReply(r).kind, u"preview"_s);
    QVERIFY(WallpaperContent::fromResolveReply(r).script.isEmpty());
}

void TestWallpaperContent::webItemsListReadableRoots()
{
    QTemporaryDir tmp;
    const QString preset = tmp.filePath(u"preset"_s);
    const QString base = tmp.filePath(u"base"_s);
    const QString picked = tmp.filePath(u"mine/pic.jpg"_s);
    testutil::writeFile(preset + u"/files/a.jpg"_s, "x");
    testutil::writeFile(base + u"/index.html"_s, "x");
    testutil::writeFile(picked, "x");
    QJsonObject item{{u"id"_s, u"we:2"_s}, {u"kind"_s, u"web"_s}, {u"status"_s, u"ok"_s}, {u"dir"_s, preset},
                     {u"contentDir"_s, base}, {u"entryFile"_s, base + u"/index.html"_s},
                     {u"properties"_s, testutil::json("{\"a\": [{\"key\": \"bg\", \"type\": \"file\"},"
                                                      " {\"key\": \"text\", \"type\": \"textinput\"}]}")
                                           .value(u"a"_s)}};
    const QJsonObject assignment{{u"itemId"_s, u"we:2"_s},
                                 {u"properties"_s, QJsonObject{{u"bg"_s, picked}, {u"text"_s, u"/etc"_s}}}};
    const WallpaperContent content =
        WallpaperContent::fromResolveReply(QJsonObject{{u"assignment"_s, assignment}, {u"item"_s, item}});
    QCOMPARE(content.kind, u"web"_s);
    QCOMPARE(content.readableRoots, (QStringList{testutil::canonical(preset), testutil::canonical(base),
                                                 testutil::canonical(picked)}));

    const WallpaperContent video = WallpaperContent::fromResolveReply(reply("video", "ok"));
    QVERIFY(video.readableRoots.isEmpty());
}

void TestWallpaperContent::sceneItemsCarryEngineInputs()
{
    const QString assets = u"/we/assets"_s;
    QJsonObject r = reply("scene", "ok", "fit", "{\"tint\": \"1 0 0.5\", \"speed\": 9}");
    QJsonObject item = r.value(u"item"_s).toObject();
    item.insert(u"properties"_s, QJsonArray{
        QJsonObject{{u"key"_s, u"speed"_s}, {u"type"_s, u"slider"_s}, {u"value"_s, 3}},
        QJsonObject{{u"key"_s, u"tint"_s}, {u"type"_s, u"color"_s}, {u"value"_s, u"0 0 0"_s}},
        QJsonObject{{u"key"_s, u"hex"_s}, {u"type"_s, u"color"_s}, {u"value"_s, u"#ff0000"_s}},
        QJsonObject{{u"key"_s, u"on"_s}, {u"type"_s, u"bool"_s}, {u"value"_s, true}},
        QJsonObject{{u"key"_s, u"title"_s}, {u"type"_s, u"label"_s}, {u"value"_s, u"x"_s}}});
    item.insert(u"presetValues"_s, QJsonObject{{u"on"_s, false}});
    r.insert(u"item"_s, item);
    const WallpaperContent content = WallpaperContent::fromResolveReply(r, assets);
    QCOMPARE(content.kind, u"scene"_s);
    QCOMPARE(content.contentDir, u"/w/1"_s);
    QCOMPARE(content.assetsDir, assets);
    QCOMPARE(content.fill, u"fit"_s);
    QVERIFY(content.reason.isEmpty());
    QCOMPARE(content.sceneProperties.value(u"speed"_s).toInt(), 9);
    QCOMPARE(content.sceneProperties.value(u"tint"_s).toString(), u"1 0 0.5"_s);
    QCOMPARE(content.sceneProperties.value(u"hex"_s).toString(), u"1 0 0"_s);
    QCOMPARE(content.sceneProperties.value(u"on"_s).toBool(true), false);
    QVERIFY(!content.sceneProperties.contains(u"title"_s));
    QVERIFY(WallpaperContent::fromResolveReply(reply("scene", "invalid"), assets).kind == u"preview"_s);
}

void TestWallpaperContent::sceneKeyIgnoresPropertiesNotAssets()
{
    const QString assets = u"/we/assets"_s;
    const QString base = WallpaperContent::fromResolveReply(reply("scene", "ok"), assets).key;
    QCOMPARE(WallpaperContent::fromResolveReply(reply("scene", "ok", "fill", "{\"speed\": 9}"), assets).key, base);
    QVERIFY(WallpaperContent::fromResolveReply(reply("scene", "ok", "fit"), assets).key != base);
    QVERIFY(WallpaperContent::fromResolveReply(reply("scene", "ok"), u"/other/assets"_s).key != base);
    // Same-key updates diff like web ones.
    const WallpaperContent a = WallpaperContent::fromResolveReply(reply("scene", "ok"), assets);
    const WallpaperContent b = WallpaperContent::fromResolveReply(reply("scene", "ok", "fill", "{\"speed\": 9}"), assets);
    QCOMPARE(changedUserProperties(a.sceneProperties, b.sceneProperties), (QJsonObject{{u"speed"_s, 9}}));
}

void TestWallpaperContent::scenePropertyLayering()
{
    const QString assets = u"/we/assets"_s;
    QJsonObject r = reply("scene", "ok", "fill",
                          "{\"speed\": 9, \"mode\": null, \"rgb\": [1, 0.5, 0], \"name\": \"Ana\"}");
    QJsonObject item = r.value(u"item"_s).toObject();
    item.insert(u"properties"_s, QJsonArray{
        QJsonObject{{u"key"_s, u"speed"_s}, {u"type"_s, u"slider"_s}, {u"value"_s, 3}},
        QJsonObject{{u"key"_s, u"mode"_s}, {u"type"_s, u"combo"_s}, {u"value"_s, u"b"_s}},
        QJsonObject{{u"key"_s, u"rgb"_s}, {u"type"_s, u"color"_s}, {u"value"_s, u"0 0 0"_s}},
        QJsonObject{{u"key"_s, u"name"_s}, {u"type"_s, u"textinput"_s}, {u"value"_s, u"x"_s}},
        QJsonObject{{u"key"_s, u"level"_s}, {u"type"_s, u"combo"_s}, {u"value"_s, 2}}});
    // The preset beats the default, the assignment beats the preset, and a null override changes nothing.
    item.insert(u"presetValues"_s, QJsonObject{{u"speed"_s, 5}, {u"mode"_s, u"c"_s}, {u"level"_s, 4}});
    r.insert(u"item"_s, item);
    const QJsonObject values = WallpaperContent::fromResolveReply(r, assets).sceneProperties;
    QCOMPARE(values.value(u"speed"_s).toInt(), 9);
    QCOMPARE(values.value(u"mode"_s).toString(), u"c"_s);
    QCOMPARE(values.value(u"rgb"_s).toString(), u"1 0.5 0"_s);
    QCOMPARE(values.value(u"name"_s).toString(), u"Ana"_s);
    QCOMPARE(values.value(u"level"_s).toInt(), 4);
    // Same key whatever the overrides, as a property edit must not reload the scene.
    QJsonObject plain = r;
    QJsonObject assignment = plain.value(u"assignment"_s).toObject();
    assignment.insert(u"properties"_s, QJsonObject());
    plain.insert(u"assignment"_s, assignment);
    QCOMPARE(WallpaperContent::fromResolveReply(plain, assets).key, WallpaperContent::fromResolveReply(r, assets).key);
}

void TestWallpaperContent::spanIsPartOfTheKey()
{
    const QJsonObject plain = reply("video", "ok");
    QVERIFY(!WallpaperContent::fromResolveReply(plain).span);
    QJsonObject spanned = plain;
    QJsonObject assignment = spanned.value(u"assignment"_s).toObject();
    assignment.insert(u"span"_s, true);
    spanned.insert(u"assignment"_s, assignment);
    const WallpaperContent content = WallpaperContent::fromResolveReply(spanned);
    QVERIFY(content.span);
    // Turning span on or off replaces the delegate; an assignment without it keeps its key from before spanning.
    QVERIFY(content.key != WallpaperContent::fromResolveReply(plain).key);
    assignment.remove(u"span"_s);
    spanned.insert(u"assignment"_s, assignment);
    QCOMPARE(WallpaperContent::fromResolveReply(spanned).key, WallpaperContent::fromResolveReply(plain).key);
    // a malformed value is not span
    assignment.insert(u"span"_s, u"yes"_s);
    spanned.insert(u"assignment"_s, assignment);
    QVERIFY(!WallpaperContent::fromResolveReply(spanned).span);
}

void TestWallpaperContent::networkPermissionIsDeniedByDefaultAndPartOfTheKey()
{
    const auto withPermission = [](const QJsonValue &network) {
        QJsonObject r = reply("web", "ok");
        QJsonObject item = r.value(u"item"_s).toObject();
        if (!network.isUndefined())
            item.insert(u"permissions"_s, QJsonObject{{u"network"_s, network}});
        r.insert(u"item"_s, item);
        return WallpaperContent::fromResolveReply(r);
    };
    const WallpaperContent missing = withPermission(QJsonValue(QJsonValue::Undefined)); // a daemon without revision 8
    QVERIFY(!missing.networkAllowed);
    QVERIFY(!withPermission(false).networkAllowed);
    QVERIFY(!withPermission(u"true"_s).networkAllowed); // only a real boolean grants
    QVERIFY(!withPermission(1).networkAllowed);
    const WallpaperContent allowed = withPermission(true);
    QVERIFY(allowed.networkAllowed);
    QVERIFY(allowed.key != withPermission(false).key); // toggling reloads the page under its new rules
}

QTEST_GUILESS_MAIN(TestWallpaperContent)
#include "tst_wallpapercontent.moc"
