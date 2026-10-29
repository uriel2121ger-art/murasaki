#include "daemonclient.h"
#include "fakebackend.h"
#include "librarymodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::DaemonClient;
using murasaki::LibraryModel;

namespace {

QJsonObject item(const QString &id, const QString &title, const QString &kind = u"video"_s,
                 const QStringList &tags = {}, const QString &preview = {})
{
    return QJsonObject{{"id", id},
                       {"title", title},
                       {"kind", kind},
                       {"status", "ok"},
                       {"statusDetail", ""},
                       {"preview", preview},
                       {"tags", QJsonArray::fromStringList(tags)},
                       {"contentRating", "Everyone"},
                       {"workshopId", id.mid(3)},
                       {"dependencyId", ""}};
}

QByteArray list(const QList<QJsonObject> &items)
{
    QJsonArray array;
    for (const QJsonObject &o : items)
        array.append(o);
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

} // namespace

class TstLibraryModel : public QObject
{
    Q_OBJECT

private:
    FakeBackend *backend = nullptr;
    DaemonClient *client = nullptr;

    void setUpClient(const QList<QJsonObject> &items)
    {
        delete client; // owns the previous backend
        backend = new FakeBackend;
        backend->setProperty(u"ApiVersion"_s, 4u);
        backend->answerJson(u"ListItems"_s, list(items));
        backend->answerJson(u"Assignments"_s, "[]");
        backend->answerJson(u"Settings"_s, "{}");
        client = new DaemonClient(backend);
    }

    void cleanup()
    {
        delete client;
        client = nullptr;
    }

private slots:
    void riskAndNetworkRolesFollowTheSummary()
    {
        QJsonObject risky = item(u"we:1"_s, u"Alpha"_s);
        risky.insert("risks", QJsonArray{QJsonObject{{"kind", "executable"}, {"file", "a.exe"}}});
        QJsonObject online = item(u"we:2"_s, u"Beta"_s, u"web"_s);
        online.insert("permissions", QJsonObject{{"network", true}});
        QJsonObject offline = item(u"we:3"_s, u"Gamma"_s, u"web"_s);
        offline.insert("permissions", QJsonObject{{"network", false}});
        setUpClient({risky, online, offline, item(u"we:4"_s, u"Delta"_s)}); // the last one: a daemon without either field
        LibraryModel model(client);
        backend->setAvailable(true);
        QTRY_COMPARE(model.rowCount(), 4);
        QVERIFY(model.data(model.index(0), LibraryModel::HasRisksRole).toBool());
        QVERIFY(!model.data(model.index(0), LibraryModel::NetworkAllowedRole).toBool());
        QVERIFY(!model.data(model.index(1), LibraryModel::HasRisksRole).toBool());
        QVERIFY(model.data(model.index(1), LibraryModel::NetworkAllowedRole).toBool());
        QVERIFY(!model.data(model.index(2), LibraryModel::NetworkAllowedRole).toBool());
        QVERIFY(!model.data(model.index(3), LibraryModel::HasRisksRole).toBool());
        QVERIFY(!model.data(model.index(3), LibraryModel::NetworkAllowedRole).toBool());
    }

    void populatesFromDaemon()
    {
        setUpClient({item(u"we:1"_s, u"Alpha"_s, u"video"_s, {u"Anime"_s, u"Cute"_s}, u"/p/alpha.jpg"_s),
                     item(u"we:2"_s, u"Beta"_s, u"web"_s)});
        LibraryModel model(client);
        QSignalSpy countSpy(&model, &LibraryModel::countChanged);
        QSignalSpy firstInsert(&model, &QAbstractItemModel::rowsInserted);
        backend->setAvailable(true);
        QTRY_COMPARE(model.rowCount(), 2);
        QCOMPARE(countSpy.size(), 1);
        QCOMPARE(firstInsert.size(), 1); // one insertion for the whole initial load
        QCOMPARE(firstInsert.first().at(2).toInt(), 1);

        const QModelIndex first = model.index(0);
        QCOMPARE(model.data(first, LibraryModel::ItemIdRole).toString(), u"we:1"_s);
        QCOMPARE(model.data(first, LibraryModel::TitleRole).toString(), u"Alpha"_s);
        QCOMPARE(model.data(first, LibraryModel::KindRole).toString(), u"video"_s);
        QCOMPARE(model.data(first, LibraryModel::StatusRole).toString(), u"ok"_s);
        QCOMPARE(model.data(first, LibraryModel::TagsRole).toStringList(), (QStringList{u"Anime"_s, u"Cute"_s}));
        QCOMPARE(model.data(first, LibraryModel::PreviewRole).toUrl(), QUrl::fromLocalFile(u"/p/alpha.jpg"_s));
        QCOMPARE(model.data(first, LibraryModel::ContentRatingRole).toString(), u"Everyone"_s);
        QCOMPARE(model.data(first, LibraryModel::WorkshopIdRole).toString(), u"1"_s);
        QVERIFY(!model.data(first, LibraryModel::AssignedRole).toBool());
        QVERIFY(model.data(model.index(1), LibraryModel::PreviewRole).toUrl().isEmpty());

        QCOMPARE(model.indexOf(u"we:2"_s), 1);
        QCOMPARE(model.indexOf(u"nope"_s), -1);
        QCOMPARE(model.get(1).value(u"title"_s).toString(), u"Beta"_s);
        QCOMPARE(model.get(1).value(u"itemId"_s).toString(), u"we:2"_s);
        QVERIFY(model.get(5).isEmpty());
    }

    void incrementalUpdatesNeverReset()
    {
        setUpClient({item(u"we:1"_s, u"Alpha"_s), item(u"we:3"_s, u"Gamma"_s), item(u"we:4"_s, u"Delta2"_s)});
        LibraryModel model(client);
        backend->setAvailable(true);
        QTRY_COMPARE(model.rowCount(), 3);

        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);

        // Insert "Beta" between Alpha and Gamma.
        backend->answerJson(u"ListItems"_s, list({item(u"we:1"_s, u"Alpha"_s), item(u"we:2"_s, u"Beta"_s),
                                                  item(u"we:3"_s, u"Gamma"_s), item(u"we:4"_s, u"Delta2"_s)}));
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{u"we:2"_s}, QStringList{}, QStringList{}});
        QTRY_COMPARE(inserted.size(), 1);
        QCOMPARE(inserted.first().at(1).toInt(), 1);
        QCOMPARE(inserted.first().at(2).toInt(), 1);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(changed.size(), 0);

        // Change one item's status.
        QJsonObject gamma = item(u"we:3"_s, u"Gamma"_s);
        gamma["status"] = "missing-dependency";
        gamma["statusDetail"] = "999";
        backend->answerJson(u"ListItems"_s, list({item(u"we:1"_s, u"Alpha"_s), item(u"we:2"_s, u"Beta"_s), gamma,
                                                  item(u"we:4"_s, u"Delta2"_s)}));
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{}, QStringList{u"we:3"_s}, QStringList{}});
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(changed.first().at(0).toModelIndex().row(), 2);
        QCOMPARE(model.data(model.index(2), LibraryModel::StatusRole).toString(), u"missing-dependency"_s);
        QCOMPARE(model.data(model.index(2), LibraryModel::StatusDetailRole).toString(), u"999"_s);

        // Remove Alpha.
        backend->answerJson(u"ListItems"_s,
                            list({item(u"we:2"_s, u"Beta"_s), gamma, item(u"we:4"_s, u"Delta2"_s)}));
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{}, QStringList{}, QStringList{u"we:1"_s}});
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(removed.first().at(1).toInt(), 0);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.indexOf(u"we:1"_s), -1);

        // Rename an item so that it sorts elsewhere: moved, not reset.
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        backend->answerJson(u"ListItems"_s,
                            list({item(u"we:2"_s, u"Beta"_s), item(u"we:4"_s, u"Delta2"_s), item(u"we:3"_s, u"Zeta"_s)}));
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{}, QStringList{u"we:3"_s}, QStringList{}});
        QTRY_COMPARE(model.indexOf(u"we:3"_s), 2);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(model.data(model.index(2), LibraryModel::TitleRole).toString(), u"Zeta"_s);

        QCOMPARE(reset.size(), 0);
    }

    void assignedRoleFollowsAssignments()
    {
        setUpClient({item(u"we:1"_s, u"Alpha"_s), item(u"we:2"_s, u"Beta"_s)});
        backend->answerJson(u"Assignments"_s, R"([{"output":"DP-1","desktop":"*","itemId":"we:2"}])");
        LibraryModel model(client);
        backend->setAvailable(true);
        QTRY_COMPARE(model.rowCount(), 2);
        QTRY_VERIFY(model.data(model.index(1), LibraryModel::AssignedRole).toBool());
        QVERIFY(!model.data(model.index(0), LibraryModel::AssignedRole).toBool());

        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        backend->answerJson(u"Assignments"_s, R"([{"output":"*","desktop":"*","itemId":"we:1"}])");
        backend->emitSignal(u"AssignmentsChanged"_s);
        QTRY_COMPARE(changed.size(), 2);
        QVERIFY(model.data(model.index(0), LibraryModel::AssignedRole).toBool());
        QVERIFY(!model.data(model.index(1), LibraryModel::AssignedRole).toBool());
        QCOMPARE(changed.first().at(2).value<QList<int>>(), QList<int>{LibraryModel::AssignedRole});
    }

    void newItemStartsWithCurrentAssignment()
    {
        setUpClient({item(u"we:1"_s, u"Alpha"_s)});
        backend->answerJson(u"Assignments"_s, R"([{"output":"*","desktop":"*","itemId":"we:2"}])");
        LibraryModel model(client);
        backend->setAvailable(true);
        QTRY_COMPARE(model.rowCount(), 1);
        QTRY_VERIFY(!client->assignments().isEmpty());

        backend->answerJson(u"ListItems"_s, list({item(u"we:1"_s, u"Alpha"_s), item(u"we:2"_s, u"Beta"_s)}));
        backend->emitSignal(u"ItemsChanged"_s, {QStringList{u"we:2"_s}, QStringList{}, QStringList{}});
        QTRY_COMPARE(model.rowCount(), 2);
        QVERIFY(model.data(model.index(1), LibraryModel::AssignedRole).toBool());
    }

    void roleNamesAvoidReservedId()
    {
        setUpClient({});
        LibraryModel model(client);
        const auto names = model.roleNames();
        QVERIFY(names.values().contains("itemId"));
        QVERIFY(!names.values().contains("id"));
    }
};

QTEST_MAIN(TstLibraryModel)
#include "tst_librarymodel.moc"
