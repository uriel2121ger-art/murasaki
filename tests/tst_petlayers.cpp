// keep above off: what each presenter can do and the name it reports (petbehaviours.h).

#include "petbehaviours.h"

#include <QTest>

#include <algorithm>

using namespace murasaki;
using degrade::PetPresenter;

class TestPetLayers : public QObject
{
    Q_OBJECT

private slots:
    void aPetThatStaysAboveReportsNothingAnywhere()
    {
        for (const PetPresenter presenter : {PetPresenter::LayerShell, PetPresenter::X11, PetPresenter::Shell})
            QVERIFY(degrade::keepAboveDegrade(true, presenter).empty());
    }
    void keepAboveOffCostsWhatEachPresenterCannotDo()
    {
        // layer-shell: the bottom layer, below every window
        QCOMPARE(degrade::keepAboveDegrade(false, PetPresenter::LayerShell), degrade::kKeepAboveBottom);
        // X11: exact, nothing to report
        QVERIFY(degrade::keepAboveDegrade(false, PetPresenter::X11).empty());
        // GNOME: the extension's marker is not checked on a real shell yet
        QCOMPARE(degrade::keepAboveDegrade(false, PetPresenter::Shell), degrade::kKeepAboveUnsupported);
    }
    void theNamesAreInTheTableOfNames()
    {
        const auto known = [](std::string_view name) {
            return std::find(degrade::kAll.begin(), degrade::kAll.end(), name) != degrade::kAll.end();
        };
        QVERIFY(known(degrade::keepAboveDegrade(false, PetPresenter::LayerShell)));
        QVERIFY(known(degrade::keepAboveDegrade(false, PetPresenter::Shell)));
    }
};

QTEST_APPLESS_MAIN(TestPetLayers)
#include "tst_petlayers.moc"
