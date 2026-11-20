// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of sessionEnergy / totalEnergyConsumed, built from ChargeEnergy, which
// the charger resets whenever a charge stops (including every pause).

#include "trydanenergy.h"

#include <QTest>

class TestTrydanEnergy : public QObject
{
    Q_OBJECT

private slots:
    void incrementRule()
    {
        QCOMPARE(TrydanEnergy::increment(2.5, 2.0), 0.5);
        QCOMPARE(TrydanEnergy::increment(2.0, 2.0), 0.0);
        QCOMPARE(TrydanEnergy::increment(1.9995, 2.0), 0.0);  // noise
        QCOMPARE(TrydanEnergy::increment(0.2, 9.0), 0.2);     // reset
    }

    void sessionSurvivesAPauseForAPhaseSwitch()
    {
        TrydanEnergy e;
        e.restore(false, 0, 0, 0);
        TrydanEnergy::Result r = e.update(0.0, true);
        QVERIFY(r.sessionOpened);
        e.update(1.0, true);
        e.update(2.0, true);
        // Pause around the switch: the charger resets ChargeEnergy.
        r = e.update(0.0, true);
        QVERIFY(r.chargerReset);
        QCOMPARE(r.sessionEnergy, 2.0);
        r = e.update(0.5, true);
        QCOMPARE(r.sessionEnergy, 2.5);
        QCOMPARE(r.totalEnergy, 2.5);
    }

    void unplugIsConfirmedOnThreeSuccessfulPolls()
    {
        TrydanEnergy e;
        e.restore(false, 0, 0, 0);
        e.update(0.0, true);
        e.update(3.0, true);
        QVERIFY(!e.update(3.0, false).sessionClosed);
        QVERIFY(!e.update(3.0, false).sessionClosed);
        // A plug-in in between restarts the count: no closing on a short absence.
        QVERIFY(!e.update(3.0, true).sessionClosed);
        QVERIFY(!e.update(3.0, false).sessionClosed);
        QVERIFY(!e.update(3.0, false).sessionClosed);
        TrydanEnergy::Result r = e.update(3.0, false);
        QVERIFY(r.sessionClosed);
        // The value stays readable after the close.
        QCOMPARE(r.sessionEnergy, 3.0);
        QCOMPARE(e.update(3.0, false).sessionEnergy, 3.0);
        // The next plug-in opens a new session from the raw value.
        r = e.update(0.0, true);
        QVERIFY(r.sessionOpened);
        QCOMPARE(r.sessionEnergy, 0.0);
        QCOMPARE(r.totalEnergy, 3.0);
    }

    void totalCountsOutsideSessions()
    {
        TrydanEnergy e;
        e.restore(false, 0, 0, 10.0);
        // A charge the plugin never saw plugged (pluggedIn false the whole time).
        e.update(0.5, false);
        QCOMPARE(e.update(1.0, false).totalEnergy, 11.0);
    }

    void restartInTheMiddleOfASession()
    {
        // nymea restarted: last raw 8.5 kWh published, session open, 8.5 in the session.
        TrydanEnergy e;
        e.restore(true, 8.5, 8.5, 20.0);
        TrydanEnergy::Result r = e.update(9.0, true);
        QVERIFY(!r.sessionOpened);
        QCOMPARE(r.sessionEnergy, 9.0);
        QCOMPARE(r.totalEnergy, 20.5);
    }

    void resetDuringTheRestartIsNotLost()
    {
        TrydanEnergy e;
        e.restore(true, 8.5, 8.5, 20.0);
        // The charger reset while nymea was down, and has counted 0.3 since.
        const TrydanEnergy::Result r = e.update(0.3, true);
        QVERIFY(r.chargerReset);
        QCOMPARE(r.sessionEnergy, 8.8);
        QCOMPARE(r.totalEnergy, 20.3);
    }
};

QTEST_GUILESS_MAIN(TestTrydanEnergy)
#include "tst_trydanenergy.moc"
