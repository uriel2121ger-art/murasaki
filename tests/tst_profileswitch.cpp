// Switching to a profile: identity matching, what is skipped, the one write of the store, and a property run over random setups.

#include "petstore.h"
#include "profileswitch.h"
#include "testutil.h"

#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

PetPlacement pet(const QString &id, const QString &item, const QString &variant, double x = 0)
{
    PetPlacement p;
    p.petId = id;
    p.itemId = item;
    p.variant = variant;
    p.output = u"*"_s;
    p.x = x;
    return p;
}

Profile profile(const QList<PetPlacement> &pets)
{
    Profile p;
    p.name = u"p"_s;
    p.pets = entriesFromPets(pets);
    return p;
}

const ProfileItemCheck everythingExists = [](const QString &, const QString &) { return QString(); };

}

class TstProfileSwitch : public QObject
{
    Q_OBJECT

private slots:
    void aPetThatExistsKeepsItsWindowAndOnlyDifferingFieldsChange()
    {
        const QList<PetPlacement> now = {pet(u"A"_s, u"i"_s, u"v"_s, 0), pet(u"B"_s, u"j"_s, u"v"_s, 5)};
        const SwitchPlan plan = planProfileSwitch(now, profile({pet({}, u"i"_s, u"v"_s, 0), pet({}, u"j"_s, u"v"_s, 99)}), everythingExists);
        QCOMPARE(plan.kept, 1);
        QCOMPARE(plan.updated, 1);
        QCOMPARE(plan.added, 0);
        QCOMPARE(plan.removed, 0);
        QCOMPARE(plan.result[0].petId, u"A"_s);
        QCOMPARE(plan.result[1].petId, u"B"_s);
        QCOMPARE(plan.result[1].x, 99.0);
    }
    void twoPetsOfOneItemMatchInOrder()
    {
        const QList<PetPlacement> now = {pet(u"A"_s, u"i"_s, u"v"_s, 1), pet(u"B"_s, u"i"_s, u"v"_s, 2)};
        const SwitchPlan plan = planProfileSwitch(now, profile({pet({}, u"i"_s, u"v"_s, 10), pet({}, u"i"_s, u"v"_s, 20), pet({}, u"i"_s, u"v"_s, 30)}), everythingExists);
        QCOMPARE(plan.result.size(), 3);
        QCOMPARE(plan.result[0].petId, u"A"_s);
        QCOMPARE(plan.result[1].petId, u"B"_s);
        QVERIFY(plan.result[2].petId.isEmpty());
        QCOMPARE(plan.added, 1);
        QCOMPARE(plan.updated, 2);
        // and fewer entries than pets: the last pet goes
        const SwitchPlan fewer = planProfileSwitch(now, profile({pet({}, u"i"_s, u"v"_s, 1)}), everythingExists);
        QCOMPARE(fewer.removed, 1);
        QCOMPARE(fewer.result[0].petId, u"A"_s);
    }
    void aChangeOfVariantKeepsThePetWhenTheItemIsTheSame()
    {
        const QList<PetPlacement> now = {pet(u"A"_s, u"i"_s, u"v1"_s)};
        const SwitchPlan plan = planProfileSwitch(now, profile({pet({}, u"i"_s, u"v2"_s)}), everythingExists);
        QCOMPARE(plan.result[0].petId, u"A"_s);
        QCOMPARE(plan.result[0].variant, u"v2"_s);
        QCOMPARE(plan.updated, 1);
        // the exact variant wins over the order
        const QList<PetPlacement> two = {pet(u"A"_s, u"i"_s, u"v1"_s), pet(u"B"_s, u"i"_s, u"v2"_s)};
        const SwitchPlan exact = planProfileSwitch(two, profile({pet({}, u"i"_s, u"v2"_s), pet({}, u"i"_s, u"v1"_s)}), everythingExists);
        QCOMPARE(exact.result[0].petId, u"B"_s);
        QCOMPARE(exact.result[1].petId, u"A"_s);
        QCOMPARE(exact.kept, 2);
    }
    void whatIsMissingIsSkippedWithAReasonAndTheRestApplies()
    {
        const ProfileItemCheck check = [](const QString &item, const QString &variant) {
            if (item == u"gone")
                return QString(kProfileItemMissing);
            if (variant == u"nope")
                return QString(kProfileVariantMissing);
            return QString();
        };
        Profile p = profile({pet({}, u"gone"_s, u"v"_s), pet({}, u"i"_s, u"nope"_s), pet({}, u"i"_s, u"v"_s)});
        const SwitchPlan plan = planProfileSwitch({pet(u"A"_s, u"gone"_s, u"v"_s)}, p, check);
        QCOMPARE(plan.skipped.size(), 2);
        QCOMPARE(plan.skipped[0].reason, QString(kProfileItemMissing));
        QCOMPARE(plan.skipped[0].slot, 0);
        QCOMPARE(plan.skipped[1].reason, QString(kProfileVariantMissing));
        QCOMPARE(plan.result.size(), 1);
        QCOMPARE(plan.added, 1);
        QCOMPARE(plan.removed, 1); // the pet whose item is gone is not kept alive by an entry that is skipped
    }
    void switchingToTheSameSetupIsANoop()
    {
        const QList<PetPlacement> now = {pet(u"A"_s, u"i"_s, u"v"_s), pet(u"B"_s, u"j"_s, u"w"_s, 4)};
        const Profile p = profile(now);
        QVERIFY(planProfileSwitch(now, p, everythingExists).isNoop());
        QVERIFY(!profileDiffers(now, p, everythingExists));
        QList<PetPlacement> moved = now;
        moved[1].x = 5;
        QVERIFY(profileDiffers(moved, p, everythingExists));
        QVERIFY(profileDiffers({}, p, everythingExists));
    }
    void theStoreAppliesTheResultInOneWrite()
    {
        QTemporaryDir tmp;
        PetStore store(tmp.filePath(u"pets.json"_s));
        const QString a = store.add(pet({}, u"i"_s, u"v"_s, 1));
        const QString b = store.add(pet({}, u"j"_s, u"v"_s, 2));
        QSignalSpy changed(&store, &PetStore::changed);
        const SwitchPlan plan = planProfileSwitch(store.pets(), profile({pet({}, u"i"_s, u"v"_s, 50), pet({}, u"k"_s, u"v"_s, 3)}), everythingExists);
        QString error;
        QVERIFY2(store.replaceAll(plan.result, &error), qPrintable(error));
        QCOMPARE(changed.size(), 1); // one signal for everything
        QCOMPARE(store.pets().size(), 2);
        QCOMPARE(store.pets()[0].petId, a); // the window stays
        QCOMPARE(store.pets()[0].x, 50.0);
        QVERIFY(!store.pet(b).has_value()); // j is gone
        QCOMPARE(store.pets()[1].itemId, u"k"_s);
        PetStore again(tmp.filePath(u"pets.json"_s));
        again.load();
        QCOMPARE(again.pets(), store.pets());
        QVERIFY(!store.replaceAll(store.pets(), &error)); // the same list: no write, no signal
        QVERIFY(error.isEmpty());
        QCOMPARE(changed.size(), 1);
    }
    void anInvalidResultOrAFailingWriteLeavesTheOldSetup()
    {
        QTemporaryDir tmp;
        PetStore store(tmp.filePath(u"pets.json"_s));
        store.add(pet({}, u"i"_s, u"v"_s, 1));
        const QList<PetPlacement> before = store.pets();
        QString error;
        QList<PetPlacement> bad = before;
        bad[0].width = 1; // below the minimum
        QVERIFY(!store.replaceAll(bad, &error));
        QVERIFY(!error.isEmpty());
        QList<PetPlacement> unknown = before;
        unknown[0].petId = u"not-mine"_s;
        QVERIFY(!store.replaceAll(unknown, &error));
        QList<PetPlacement> repeated = {before[0], before[0]};
        QVERIFY(!store.replaceAll(repeated, &error));
        QList<PetPlacement> tooMany;
        for (int i = 0; i <= kMaxPets; ++i)
            tooMany.append(pet({}, u"i"_s, u"v"_s));
        QVERIFY(!store.replaceAll(tooMany, &error));
        QCOMPARE(store.pets(), before);
        // the write fails: the directory cannot be written
        QVERIFY(QFile::setPermissions(tmp.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QList<PetPlacement> moved = before;
        moved[0].x = 77;
        const bool done = store.replaceAll(moved, &error);
        QFile::setPermissions(tmp.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (!done) { // (a user who can write anyway, such as root, is not a failure of the store)
            QVERIFY(!error.isEmpty());
            QCOMPARE(store.pets(), before);
        }
    }
    void aFullProfileOfSixtyFourPetsSwitchesAtOnce()
    {
        QList<PetPlacement> many;
        for (int i = 0; i < kMaxPets; ++i)
            many.append(pet(u"p%1"_s.arg(i), u"i%1"_s.arg(i % 5), u"v"_s, i));
        Profile p = profile(many);
        for (ProfileEntry &e : p.pets)
            e.pet.x += 1;
        const SwitchPlan plan = planProfileSwitch(many, p, everythingExists);
        QCOMPARE(plan.updated, kMaxPets);
        QCOMPARE(plan.result.size(), kMaxPets);
        QCOMPARE(plan.removed, 0);
    }
    void propertyRandomSetupsGiveValidResultsThatKeepIdentities()
    {
        QRandomGenerator rng(1022);
        const auto random = [&](bool withIds) {
            QList<PetPlacement> list;
            const int n = rng.bounded(0, 12);
            for (int i = 0; i < n; ++i) {
                PetPlacement p = pet(withIds ? u"id%1"_s.arg(rng.bounded(1000000)) : QString(), u"item%1"_s.arg(rng.bounded(3)), u"var%1"_s.arg(rng.bounded(2)), rng.bounded(500));
                p.width = 16 + rng.bounded(300);
                p.rotation = rng.bounded(360);
                list.append(p);
            }
            return list;
        };
        for (int round = 0; round < 500; ++round) {
            QList<PetPlacement> now = random(true);
            QSet<QString> ids;
            for (PetPlacement &p : now) { // (distinct ids, as a store has)
                while (ids.contains(p.petId))
                    p.petId += u"x"_s;
                ids.insert(p.petId);
            }
            const Profile target = profile(random(false));
            const ProfileItemCheck check = [&](const QString &item, const QString &) { return item == u"item2" && rng.bounded(4) == 0 ? QString(kProfileItemMissing) : QString(); };
            const SwitchPlan plan = planProfileSwitch(now, target, check);
            QCOMPARE(plan.result.size(), plan.entrySlots.size());
            QCOMPARE(plan.kept + plan.updated + plan.added, plan.result.size());
            QCOMPARE(plan.kept + plan.updated + plan.removed, int(now.size()));
            QSet<QString> seen;
            for (const PetPlacement &p : plan.result) {
                QVERIFY(validatePlacement(p));
                if (!p.petId.isEmpty()) {
                    QVERIFY(ids.contains(p.petId)); // only pets that exist keep an id
                    QVERIFY(!seen.contains(p.petId));
                    seen.insert(p.petId);
                }
            }
            QCOMPARE(plan.result.size() + plan.skipped.size(), int(target.pets.size()));
        }
    }
};

QTEST_GUILESS_MAIN(TstProfileSwitch)
#include "tst_profileswitch.moc"
