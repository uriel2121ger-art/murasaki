#include "daemonclient.h"
#include "fakebackend.h"
#include "libraryfiltermodel.h"
#include "librarymodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::DaemonClient;
using murasaki::LibraryFilterModel;
using murasaki::LibraryModel;

namespace {

QJsonObject item(const QString &id, const QString &title, const QString &kind, const QStringList &tags = {})
{
    return QJsonObject{{"id", id}, {"title", title}, {"kind", kind}, {"tags", QJsonArray::fromStringList(tags)}};
}

} // namespace

class TstLibraryFilterModel : public QObject
{
    Q_OBJECT

private:
    // Shuffled on purpose: the proxy has to do the sorting.
    QJsonArray items() const
    {
        return {item(u"we:1"_s, u"zebra stripes"_s, u"video"_s, {u"Animal"_s}),
                item(u"we:2"_s, u"Pokémon Forest"_s, u"scene"_s, {u"Anime"_s, u"Games"_s}),
                item(u"we:3"_s, u"alpha"_s, u"web"_s, {u"Sci-Fi"_s}),
                item(u"we:4"_s, u"Beta Wave"_s, u"video"_s, {u"Música"_s})};
    }

    QStringList titles(const LibraryFilterModel &model) const
    {
        QStringList out;
        for (int i = 0; i < model.rowCount(); ++i)
            out << model.data(model.index(i, 0), LibraryModel::TitleRole).toString();
        return out;
    }

private slots:
    void sortsByTitleAndFilters()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->answerJson(u"ListItems"_s, QJsonDocument(items()).toJson(QJsonDocument::Compact));
        backend->answerJson(u"Assignments"_s, "[]");
        backend->answerJson(u"Settings"_s, "{}");
        DaemonClient client(backend);
        LibraryModel library(&client);
        LibraryFilterModel filter;
        filter.setSourceModel(&library);
        QSignalSpy countSpy(&filter, &LibraryFilterModel::countChanged);
        backend->setAvailable(true);
        QTRY_COMPARE(filter.count(), 4);
        QVERIFY(countSpy.size() >= 1);

        // Case-insensitive, locale-aware order regardless of the daemon's order.
        QCOMPARE(titles(filter), (QStringList{u"alpha"_s, u"Beta Wave"_s, u"Pokémon Forest"_s, u"zebra stripes"_s}));

        // Accent and case insensitive search over the title.
        filter.setSearchText(u"pokemon"_s);
        QCOMPARE(titles(filter), QStringList{u"Pokémon Forest"_s});
        QCOMPARE(filter.count(), 1);
        filter.setSearchText(u"POKÉMON"_s);
        QCOMPARE(filter.count(), 1);

        // Search over tags, also accent insensitive.
        filter.setSearchText(u"musica"_s);
        QCOMPARE(titles(filter), QStringList{u"Beta Wave"_s});
        filter.setSearchText(u"anime"_s);
        QCOMPARE(titles(filter), QStringList{u"Pokémon Forest"_s});

        // Proxy-aware accessors.
        filter.setSearchText(u"musica"_s);
        QCOMPARE(filter.itemIdAt(0), u"we:4"_s);
        QCOMPARE(filter.get(0).value(u"title"_s).toString(), u"Beta Wave"_s);
        QVERIFY(filter.get(1).isEmpty());
        QVERIFY(filter.itemIdAt(-1).isEmpty());

        // Words may be separated by any whitespace.
        filter.setSearchText(u"forest\t  games"_s);
        QCOMPARE(filter.count(), 1);

        // Every word has to match, anywhere in title or tags.
        filter.setSearchText(u"forest games"_s);
        QCOMPARE(filter.count(), 1);
        filter.setSearchText(u"forest zebra"_s);
        QCOMPARE(filter.count(), 0);

        // Kind filter, alone and combined with search.
        filter.setSearchText({});
        filter.setKindFilter(u"video"_s);
        QCOMPARE(titles(filter), (QStringList{u"Beta Wave"_s, u"zebra stripes"_s}));
        filter.setSearchText(u"zebra"_s);
        QCOMPARE(titles(filter), QStringList{u"zebra stripes"_s});
        filter.setKindFilter(u"scene"_s);
        QCOMPARE(filter.count(), 0);
        filter.setSearchText({});
        QCOMPARE(titles(filter), QStringList{u"Pokémon Forest"_s});
        filter.setKindFilter({});
        QCOMPARE(filter.count(), 4);
    }

    // Editor asset packs are not wallpapers: the library never lists them (only asking for the kind does).
    void hidesAssetPacks()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->answerJson(u"ListItems"_s, QJsonDocument(QJsonArray{item(u"we:1"_s, u"clip"_s, u"video"_s, {}),
                                                                   item(u"we:2"_s, u"Clock pack"_s, u"asset"_s, {})})
                                                .toJson(QJsonDocument::Compact));
        backend->answerJson(u"Assignments"_s, "[]");
        backend->answerJson(u"Settings"_s, "{}");
        DaemonClient client(backend);
        LibraryModel library(&client);
        LibraryFilterModel filter;
        filter.setSourceModel(&library);
        backend->setAvailable(true);
        QTRY_COMPARE(library.rowCount(), 2);
        QCOMPARE(titles(filter), QStringList{u"clip"_s});
        filter.setSearchText(u"clock"_s);
        QCOMPARE(filter.count(), 0);
        filter.setSearchText({});
        filter.setKindFilter(u"asset"_s);
        QCOMPARE(titles(filter), QStringList{u"Clock pack"_s});
    }

    void foldRemovesAccentsAndCase()
    {
        QCOMPARE(LibraryFilterModel::fold(u"Pokémon"_s), u"pokemon"_s);
        QCOMPARE(LibraryFilterModel::fold(u"ÑANDÚ Über"_s), u"nandu uber"_s);
    }

    void followsLibraryChangesWithoutReset()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->answerJson(u"ListItems"_s, QJsonDocument(items()).toJson(QJsonDocument::Compact));
        backend->answerJson(u"Assignments"_s, "[]");
        backend->answerJson(u"Settings"_s, "{}");
        DaemonClient client(backend);
        LibraryModel library(&client);
        LibraryFilterModel filter;
        filter.setSourceModel(&library);
        backend->setAvailable(true);
        QTRY_COMPARE(filter.count(), 4);

        QSignalSpy reset(&filter, &QAbstractItemModel::modelReset);
        QJsonArray more = items();
        more.append(item(u"we:5"_s, u"Aardvark"_s, u"web"_s));
        backend->answerJson(u"ListItems"_s, QJsonDocument(more).toJson(QJsonDocument::Compact));
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{u"we:5"_s}, QStringList{}, QStringList{}});
        QTRY_COMPARE(filter.count(), 5);
        QCOMPARE(titles(filter).first(), u"Aardvark"_s);
        QCOMPARE(reset.size(), 0);
    }

    void petKindFilterListsOnlyPets()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 5u);
        QJsonArray list = items();
        list.append(item(u"anima:7"_s, u"Teto"_s, u"pet"_s, {u"Pet"_s}));
        list.append(item(u"local:abc"_s, u"Cat"_s, u"pet"_s));
        backend->answerJson(u"ListItems"_s, QJsonDocument(list).toJson(QJsonDocument::Compact));
        backend->answerJson(u"Assignments"_s, "[]");
        backend->answerJson(u"Settings"_s, "{}");
        backend->answerJson(u"ListPets"_s, "[]");
        DaemonClient client(backend);
        LibraryModel library(&client);
        LibraryFilterModel filter;
        filter.setSourceModel(&library);
        backend->setAvailable(true);
        QTRY_COMPARE(filter.count(), 6);

        filter.setKindFilter(u"pet"_s);
        QCOMPARE(titles(filter), (QStringList{u"Cat"_s, u"Teto"_s}));
        QCOMPARE(filter.get(0).value(u"kind"_s).toString(), u"pet"_s);
        filter.setSearchText(u"tet"_s);
        QCOMPARE(titles(filter), QStringList{u"Teto"_s});
        filter.setKindFilter(u"video"_s);
        QCOMPARE(filter.count(), 0);
        filter.setSearchText(QString());
        QCOMPARE(filter.count(), 2);
    }

    // "On screen" keeps only the items an assignment names, and follows the assignments as they change.
    void onScreenOnlyListsAssignedItems()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->answerJson(u"ListItems"_s, QJsonDocument(items()).toJson(QJsonDocument::Compact));
        backend->answerJson(u"Assignments"_s, R"([{"output":"DP-1","desktop":"*","itemId":"we:3"}])");
        backend->answerJson(u"Settings"_s, "{}");
        DaemonClient client(backend);
        LibraryModel library(&client);
        LibraryFilterModel filter;
        filter.setSourceModel(&library);
        backend->setAvailable(true);
        QTRY_COMPARE(filter.count(), 4);
        QTRY_VERIFY(!client.assignments().isEmpty());

        QSignalSpy onScreenSpy(&filter, &LibraryFilterModel::onScreenOnlyChanged);
        filter.setOnScreenOnly(true);
        QVERIFY(filter.onScreenOnly());
        QCOMPARE(onScreenSpy.size(), 1);
        QCOMPARE(titles(filter), QStringList{u"alpha"_s});
        filter.setOnScreenOnly(true);
        QCOMPARE(onScreenSpy.size(), 1);

        // Combined with the kind filter and the search.
        filter.setKindFilter(u"video"_s);
        QCOMPARE(filter.count(), 0);
        filter.setKindFilter({});
        filter.setSearchText(u"zebra"_s);
        QCOMPARE(filter.count(), 0);
        filter.setSearchText({});

        // A new assignment shows up without touching the filter.
        backend->answerJson(u"Assignments"_s, R"([{"output":"*","desktop":"*","itemId":"we:1"},{"output":"DP-1","desktop":"*","itemId":"we:3"}])");
        backend->emitSignal(u"AssignmentsChanged"_s);
        QTRY_COMPARE(filter.count(), 2);
        QCOMPARE(titles(filter), (QStringList{u"alpha"_s, u"zebra stripes"_s}));

        filter.setOnScreenOnly(false);
        QCOMPARE(filter.count(), 4);
    }

    // What is on a screen comes first (by title among itself), and the order follows the assignments as they change.
    void itemsOnScreenComeFirst()
    {
        auto *backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->answerJson(u"ListItems"_s, QJsonDocument(items()).toJson(QJsonDocument::Compact));
        backend->answerJson(u"Assignments"_s, R"([{"output":"DP-1","desktop":"*","itemId":"we:1"}])");
        backend->answerJson(u"Settings"_s, "{}");
        DaemonClient client(backend);
        LibraryModel library(&client);
        LibraryFilterModel filter;
        filter.setSourceModel(&library);
        backend->setAvailable(true);
        QTRY_COMPARE(filter.count(), 4);
        QTRY_COMPARE(titles(filter),
                     (QStringList{u"zebra stripes"_s, u"alpha"_s, u"Beta Wave"_s, u"Pokémon Forest"_s}));

        backend->answerJson(u"Assignments"_s, R"([{"output":"*","desktop":"*","itemId":"we:4"},{"output":"DP-1","desktop":"*","itemId":"we:2"}])");
        backend->emitSignal(u"AssignmentsChanged"_s);
        QTRY_COMPARE(titles(filter),
                     (QStringList{u"Beta Wave"_s, u"Pokémon Forest"_s, u"alpha"_s, u"zebra stripes"_s}));

        // The search keeps the same order.
        filter.setSearchText(u"a"_s);
        QCOMPARE(titles(filter).first(), u"Beta Wave"_s);
    }
};

QTEST_MAIN(TstLibraryFilterModel)
#include "tst_libraryfiltermodel.moc"
