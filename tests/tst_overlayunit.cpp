#include "overlayunit.h"

#include <QtTest>

using namespace murasaki;

class TstOverlayUnit : public QObject
{
    Q_OBJECT
private slots:
    void systemdWhenItIsOnTheBus() { QCOMPARE(chooseOverlayStartPath(true), OverlayStartPath::Systemd); }
    // The Flatpak sandbox (and any session without systemd): the bus activates the overlay's service name.
    void activationWhenSystemdIsAbsent() { QCOMPARE(chooseOverlayStartPath(false), OverlayStartPath::Activation); }
};

QTEST_GUILESS_MAIN(TstOverlayUnit)
#include "tst_overlayunit.moc"
