#include "pathutil.h"
#include "testutil.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestPathUtil : public QObject
{
    Q_OBJECT

private slots:
    void confinedPathRejectsEscapes();
    void containedExistingFileFindsFilesInside();
    void containedExistingFileRejectsSymlinkEscapes();
    void containedExistingFileRejectsSymlinkedDirsAndPrefixSiblings();
    void insideAnyRootMatchesRootsAndDescendantsOnly();
};

void TestPathUtil::confinedPathRejectsEscapes()
{
    const QString dir = u"/data/item"_s;
    QCOMPARE(confinedPath(dir, u"index.html"_s), u"/data/item/index.html"_s);
    QCOMPARE(confinedPath(dir, u"js/../index.html"_s), u"/data/item/index.html"_s);
    QVERIFY(confinedPath(dir, QString()).isEmpty());
    QVERIFY(confinedPath(dir, u"/etc/passwd"_s).isEmpty());
    QVERIFY(confinedPath(dir, u"../other/x"_s).isEmpty());
    QVERIFY(confinedPath(dir, u"js/../../x"_s).isEmpty());
    QVERIFY(confinedPath(dir, u"../item2/x"_s).isEmpty());
    QVERIFY(confinedPath(dir, u"a/.."_s).isEmpty()); // resolves to the dir itself
}

void TestPathUtil::containedExistingFileFindsFilesInside()
{
    QTemporaryDir tmp;
    const QString item = tmp.filePath(u"item"_s);
    const QString script = item + u"/Arthesian Library/Audio/WE_Handler.js"_s;
    testutil::writeFile(script, "x");
    testutil::writeFile(item + u"/index.html"_s, "x");

    QCOMPARE(containedExistingFile(item, u"Arthesian Library/Audio/WE_Handler.js"_s), testutil::canonical(script));
    QVERIFY(containedExistingFile(item, u"missing.js"_s).isEmpty());
    QVERIFY(containedExistingFile(item, u"Arthesian Library"_s).isEmpty()); // a directory, not a file
    QVERIFY(containedExistingFile(item, u"../item/../../x"_s).isEmpty());
    QVERIFY(containedExistingFile(tmp.filePath(u"missing-dir"_s), u"index.html"_s).isEmpty());

    // Steam libraries are often reached through a symlinked dir: the dir itself may be a link.
    QVERIFY(QFile::link(item, tmp.filePath(u"item-link"_s)));
    QCOMPARE(containedExistingFile(tmp.filePath(u"item-link"_s), u"index.html"_s),
             testutil::canonical(item + u"/index.html"_s));
}

void TestPathUtil::containedExistingFileRejectsSymlinkEscapes()
{
    QTemporaryDir tmp;
    const QString item = tmp.filePath(u"item"_s);
    testutil::writeFile(item + u"/index.html"_s, "x");
    testutil::writeFile(tmp.filePath(u"secret.txt"_s), "secret");

    QVERIFY(QFile::link(tmp.filePath(u"secret.txt"_s), item + u"/leak.txt"_s));
    QVERIFY(containedExistingFile(item, u"leak.txt"_s).isEmpty());

    QVERIFY(QFile::link(item + u"/index.html"_s, item + u"/alias.html"_s));
    QCOMPARE(containedExistingFile(item, u"alias.html"_s), testutil::canonical(item + u"/index.html"_s));
}

void TestPathUtil::containedExistingFileRejectsSymlinkedDirsAndPrefixSiblings()
{
    QTemporaryDir tmp;
    const QString item = tmp.filePath(u"item"_s);
    testutil::writeFile(item + u"/index.html"_s, "x");
    testutil::writeFile(tmp.filePath(u"outside/secret.txt"_s), "secret");
    testutil::writeFile(tmp.filePath(u"item2/secret"_s), "secret");

    QVERIFY(QFile::link(tmp.filePath(u"outside"_s), item + u"/sub"_s));
    QVERIFY(containedExistingFile(item, u"sub/secret.txt"_s).isEmpty());

    // "item2" shares the "item" prefix: a plain startsWith(dir) check would accept it.
    QVERIFY(QFile::link(tmp.filePath(u"item2/secret"_s), item + u"/x"_s));
    QVERIFY(containedExistingFile(item, u"x"_s).isEmpty());
}

void TestPathUtil::insideAnyRootMatchesRootsAndDescendantsOnly()
{
    const QStringList roots{u"/w/1"_s, u"/w/2/"_s, QString()};
    QVERIFY(isInsideAnyRoot(u"/w/1"_s, roots));
    QVERIFY(isInsideAnyRoot(u"/w/1/files/a.jpg"_s, roots));
    QVERIFY(isInsideAnyRoot(u"/w/2/x"_s, roots));
    QVERIFY(!isInsideAnyRoot(u"/w/10/x"_s, roots)); // prefix sibling
    QVERIFY(!isInsideAnyRoot(u"/w"_s, roots));
    QVERIFY(!isInsideAnyRoot(QString(), roots));
    QVERIFY(!isInsideAnyRoot(u"/etc/passwd"_s, {}));
}

QTEST_GUILESS_MAIN(TestPathUtil)
#include "tst_pathutil.moc"
