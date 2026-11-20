// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of the RealTimeData parsing and of the rules that turn one reading into
// the published states. No network: the readings are built here.

#include "trydanrealtimedata.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QtNumeric>

using namespace TrydanModel;

namespace {

QJsonObject baseFields()
{
    QJsonObject o;
    o.insert("ID", "TEST01");
    o.insert("ChargeState", 2);
    o.insert("ChargePower", 4140.0);
    o.insert("ChargeEnergy", 1.5);
    o.insert("Paused", 0);
    o.insert("Locked", 0);
    o.insert("Intensity", 6);
    o.insert("MinIntensity", 6);
    o.insert("MaxIntensity", 20);
    o.insert("Dynamic", 0);
    o.insert("PauseDynamic", 0);
    o.insert("FirmwareVersion", "9.9.9");
    o.insert("ChargeMode", 1);
    for (int i = 1; i <= 3; ++i) {
        o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 6.0);
        o.insert(QStringLiteral("VoltageMeasure_L%1").arg(i), 230.0);
    }
    return o;
}

TrydanRealTimeData parsed(const QJsonObject &o)
{
    TrydanRealTimeData data;
    if (!TrydanRealTimeData::parse(QJsonDocument(o).toJson(), &data))
        qFatal("test reading does not parse");
    return data;
}

}

class TestTrydanModel : public QObject
{
    Q_OBJECT

private slots:
    void parsesEveryField()
    {
        const TrydanRealTimeData d = parsed(baseFields());
        QCOMPARE(d.id, QStringLiteral("TEST01"));
        QCOMPARE(d.firmwareVersion, QStringLiteral("9.9.9"));
        QCOMPARE(*d.chargeState, 2);
        QCOMPARE(*d.chargePower, 4140.0);
        QCOMPARE(*d.chargeEnergy, 1.5);
        QCOMPARE(*d.intensity, 6);
        QCOMPARE(*d.maxIntensity, 20);
        QCOMPARE(*d.chargeMode, 1);
        QCOMPARE(*d.intensityMeasure[2], 6.0);
        QCOMPARE(*d.voltageMeasure[0], 230.0);
        QVERIFY(d.looksLikeTrydan());
    }

    void rejectsWhatIsNotJson()
    {
        TrydanRealTimeData d;
        QString error;
        QVERIFY(!TrydanRealTimeData::parse("<html>router</html>", &d, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!TrydanRealTimeData::parse("[1,2]", &d));
    }

    void anotherDeviceIsNotATrydan()
    {
        // Some JSON from another device: parses, but does not pass the fingerprint.
        TrydanRealTimeData d;
        QVERIFY(TrydanRealTimeData::parse(R"({"status":"ok","power":12})", &d));
        QVERIFY(!d.looksLikeTrydan());

        QJsonObject o = baseFields();
        o.remove("Paused");
        QVERIFY(!parsed(o).looksLikeTrydan());
    }

    void absentFieldsStayAbsent()
    {
        // Firmware without the per-phase fields nor ChargeMode: absent, not zero.
        QJsonObject o = baseFields();
        for (int i = 1; i <= 3; ++i) {
            o.remove(QStringLiteral("IntensityMeasure_L%1").arg(i));
            o.remove(QStringLiteral("VoltageMeasure_L%1").arg(i));
        }
        o.remove("ChargeMode");
        const TrydanRealTimeData d = parsed(o);
        QVERIFY(!d.chargeMode);
        QVERIFY(!d.intensityMeasure[0]);
        QVERIFY(!phaseMeasurementsAvailable(d));
        QCOMPARE(phaseCurrents(d), (QList<double>{0, 0, 0}));
        QVERIFY(d.looksLikeTrydan());
    }

    void numericStringsAreRead()
    {
        QJsonObject o = baseFields();
        o.insert("Intensity", "12");
        o.insert("ChargePower", " 2760.5 ");
        const TrydanRealTimeData d = parsed(o);
        QCOMPARE(*d.intensity, 12);
        QCOMPARE(*d.chargePower, 2760.5);
    }

    void writableValueByName()
    {
        const TrydanRealTimeData d = parsed(baseFields());
        QCOMPARE(*d.writableValue("Paused"), 0);
        QCOMPARE(*d.writableValue("Intensity"), 6);
        QCOMPARE(*d.writableValue("ChargeMode"), 1);
        QCOMPARE(*d.writableValue("PauseDynamic"), 0);
        QVERIFY(!d.writableValue("Dynamic"));
        QVERIFY(!d.writableValue("Nonsense"));
    }

    void pluggedInFollowsChargeState()
    {
        // No current flowing: ChargeState decides (a current flowing is a vehicle, see
        // powerFromTheCurrentsWhenAbsentOrContradicted()).
        QJsonObject o = baseFields();
        for (int i = 1; i <= 3; ++i)
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);
        o.insert("ChargeState", 0);
        QVERIFY(!pluggedIn(parsed(o), true));
        o.insert("ChargeState", 1);
        QVERIFY(pluggedIn(parsed(o), false));
        o.insert("ChargeState", 2);
        QVERIFY(pluggedIn(parsed(o), false));
        // Unknown value: not an unplug, the previous value holds.
        o.insert("ChargeState", 7);
        QVERIFY(pluggedIn(parsed(o), true));
        QVERIFY(!pluggedIn(parsed(o), false));
    }

    void chargingComesFromThePowerNotFromChargeState()
    {
        QJsonObject o = baseFields();
        QVERIFY(charging(parsed(o)));

        // Paused: the firmware keeps ChargeState at 2, but nothing flows.
        o.insert("Paused", 1);
        o.insert("ChargePower", 0.0);
        for (int i = 1; i <= 3; ++i)
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);
        QCOMPARE(*parsed(o).chargeState, 2);
        QVERIFY(!charging(parsed(o)));

        // A few watts of standby is not charging.
        o.insert("ChargePower", 40.0);
        QVERIFY(!charging(parsed(o)));
        QCOMPARE(publishedPower(parsed(o)).watts, 40.0);

        // No vehicle and no current, whatever the power field says.
        o.insert("ChargeState", 0);
        o.insert("ChargePower", 500.0);
        QVERIFY(!charging(parsed(o)));

        // Same without the per-phase fields: ChargePower is all there is.
        for (int i = 1; i <= 3; ++i) {
            o.remove(QStringLiteral("IntensityMeasure_L%1").arg(i));
            o.remove(QStringLiteral("VoltageMeasure_L%1").arg(i));
        }
        QVERIFY(!charging(parsed(o)));
        o.insert("ChargeState", 2);
        QVERIFY(charging(parsed(o)));
        QCOMPARE(publishedPower(parsed(o)).source, PowerSource::ChargePower);
        QCOMPARE(publishedPower(parsed(o)).watts, 500.0);
    }

    // Read back right after Paused=1, single phase at 15 A (on-site test): ChargePower has
    // already dropped to about the 6 A setpoint, the L1 current is still the 15 A the
    // car draws. Voltages are not in the trial log: 230 V on L1 assumed, L2/L3 without
    // voltage as the single-phase mode leaves them (the wiring check read so).
    void pauseReadBackSinglePhasePowerAheadOfTheCurrent()
    {
        QJsonObject o = baseFields();
        o.insert("ChargeState", 2);
        o.insert("Paused", 1);
        o.insert("Intensity", 6);
        o.insert("ChargeMode", 0);
        o.insert("ChargePower", 1332.0);
        o.insert("IntensityMeasure_L1", 14.94);
        o.insert("IntensityMeasure_L2", 0.0);
        o.insert("IntensityMeasure_L3", 0.0);
        o.insert("VoltageMeasure_L2", 0.0);
        o.insert("VoltageMeasure_L3", 0.0);
        const TrydanRealTimeData d = parsed(o);

        const PublishedPower power = publishedPower(d);
        QVERIFY(charging(d));
        QCOMPARE(power.watts, 14.94 * 230.0);
        QCOMPARE(power.source, PowerSource::PhaseMeasurements);
        QCOMPARE(phaseCurrents(d).at(0), 14.94);
        QCOMPARE(phaseCount(d, 3), 1u);
    }

    // Read back right after Paused=1, three phases at 15 A (on-site test), then after
    // ChargeMode=0 in the same second: ChargePower already 0, the three currents still
    // there - and the house meter saw the 11 kW too.
    void pauseReadBackThreePhasesPowerZeroWhileCurrentFlows()
    {
        QJsonObject o = baseFields(); // 230 V on L1..L3
        o.insert("ChargeState", 2);
        o.insert("Paused", 1);
        o.insert("Intensity", 6);
        o.insert("ChargeMode", 1);
        o.insert("ChargePower", 0.0);
        o.insert("IntensityMeasure_L1", 15.20);
        o.insert("IntensityMeasure_L2", 15.41);
        o.insert("IntensityMeasure_L3", 15.62);
        TrydanRealTimeData d = parsed(o);

        PublishedPower power = publishedPower(d);
        QVERIFY(charging(d));
        QCOMPARE(power.watts, (15.20 + 15.41 + 15.62) * 230.0);
        QCOMPARE(power.source, PowerSource::PhaseMeasurements);
        QVERIFY(pluggedIn(d, false));
        QCOMPARE(phaseCount(d, 3), 3u);

        // The mode already reads single phase; three phases still carry the current.
        o.insert("ChargeMode", 0);
        o.insert("IntensityMeasure_L1", 15.22);
        o.insert("IntensityMeasure_L2", 15.46);
        o.insert("IntensityMeasure_L3", 15.59);
        d = parsed(o);
        power = publishedPower(d);
        QCOMPARE(power.source, PowerSource::PhaseMeasurements);
        QCOMPARE(power.watts, (15.22 + 15.46 + 15.59) * 230.0);
        QVERIFY(charging(d));
        QCOMPARE(phaseCount(d, 3), 3u);

        // The next reading: nothing flows any more, everything says so.
        for (int i = 1; i <= 3; ++i) {
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);
        }
        d = parsed(o);
        QCOMPARE(publishedPower(d).watts, 0.0);
        QVERIFY(!charging(d));
        QCOMPARE(phaseCount(d, 3), 1u);
    }

    // Steady charge (values of the same trial): ChargePower and the currents agree within
    // power factor and voltage, and ChargePower is kept as read.
    void steadyChargeKeepsChargePower()
    {
        QJsonObject o = baseFields();
        o.insert("ChargeMode", 0);
        o.insert("VoltageMeasure_L2", 0.0);
        o.insert("VoltageMeasure_L3", 0.0);
        o.insert("IntensityMeasure_L2", 0.0);
        o.insert("IntensityMeasure_L3", 0.0);
        o.insert("ChargePower", 3350.42);
        o.insert("IntensityMeasure_L1", 15.02);
        QCOMPARE(publishedPower(parsed(o)).source, PowerSource::ChargePower);
        QCOMPARE(publishedPower(parsed(o)).watts, 3350.42);
        // Start at 6 A.
        o.insert("ChargePower", 1258.9);
        o.insert("IntensityMeasure_L1", 5.6);
        QCOMPARE(publishedPower(parsed(o)).watts, 1258.9);

        o = baseFields();
        o.insert("ChargePower", 10243.18);
        o.insert("IntensityMeasure_L1", 15.11);
        o.insert("IntensityMeasure_L2", 15.44);
        o.insert("IntensityMeasure_L3", 15.59);
        QCOMPARE(publishedPower(parsed(o)).source, PowerSource::ChargePower);
        QCOMPARE(publishedPower(parsed(o)).watts, 10243.18);
    }

    void powerFromTheCurrentsWhenAbsentOrContradicted()
    {
        // No ChargePower: from the currents.
        QJsonObject o = baseFields(); // 6 A on L1..L3 at 230 V
        o.remove("ChargePower");
        QCOMPARE(publishedPower(parsed(o)).source, PowerSource::PhaseMeasurements);
        QCOMPARE(publishedPower(parsed(o)).watts, 3 * 6.0 * 230.0);
        QVERIFY(charging(parsed(o)));

        // A power with no current on any phase: the currents make faith.
        o = baseFields();
        o.insert("ChargePower", 1332.0);
        for (int i = 1; i <= 3; ++i)
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);
        QCOMPARE(publishedPower(parsed(o)).watts, 0.0);
        QVERIFY(!charging(parsed(o)));

        // A small current against a ChargePower of 0: on the other side of the threshold,
        // so not kept even though within PowerAgreementW.
        o.insert("ChargePower", 0.0);
        o.insert("IntensityMeasure_L1", 1.0);
        QCOMPARE(publishedPower(parsed(o)).watts, 230.0);
        QVERIFY(charging(parsed(o)));

        // A phase carrying current without mains voltage on its reading: nominal voltage.
        o = baseFields();
        o.insert("ChargePower", 0.0);
        o.insert("IntensityMeasure_L1", 0.0);
        o.insert("IntensityMeasure_L2", 10.0);
        o.insert("IntensityMeasure_L3", 0.0);
        o.insert("VoltageMeasure_L2", 0.0);
        QCOMPARE(publishedPower(parsed(o)).watts, 10.0 * NominalVoltageV);

        // A current flowing with ChargeState 0: no current flows without a vehicle.
        o = baseFields();
        o.insert("ChargeState", 0);
        QVERIFY(pluggedIn(parsed(o), false));
        QVERIFY(charging(parsed(o)));

        // Neither ChargePower nor the per-phase fields: nothing to publish.
        o = baseFields();
        o.remove("ChargePower");
        o.remove("VoltageMeasure_L1");
        QCOMPARE(publishedPower(parsed(o)).source, PowerSource::None);
        QVERIFY(!charging(parsed(o)));
    }

    void openPhasesReadExactlyZero()
    {
        QJsonObject o = baseFields();
        o.insert("ChargeMode", 0);
        o.insert("ChargePower", 1380.0);
        o.insert("IntensityMeasure_L1", 5.9);
        o.insert("IntensityMeasure_L2", 0.3);
        o.insert("IntensityMeasure_L3", 0.99);
        const TrydanRealTimeData d = parsed(o);
        const QList<double> currents = phaseCurrents(d);
        QCOMPARE(currents.at(0), 5.9);
        QCOMPARE(currents.at(1), 0.0);
        QCOMPARE(currents.at(2), 0.0);
        QCOMPARE(phaseCount(d, 3), 1u);
    }

    void phaseCountWhileCharging()
    {
        QJsonObject o = baseFields();
        QCOMPARE(phaseCount(parsed(o), 3), 3u);

        // Mixed mode: the charger picks; the currents say what it does.
        o.insert("ChargeMode", 2);
        o.insert("IntensityMeasure_L2", 0.0);
        o.insert("IntensityMeasure_L3", 0.0);
        QCOMPARE(phaseCount(parsed(o), 3), 1u);
        o.insert("IntensityMeasure_L2", 6.0);
        o.insert("IntensityMeasure_L3", 6.0);
        QCOMPARE(phaseCount(parsed(o), 3), 3u);

        // A single-phase car on a three-phase charger draws on one phase: that is the fact.
        o.insert("ChargeMode", 1);
        o.insert("IntensityMeasure_L2", 0.2);
        o.insert("IntensityMeasure_L3", 0.2);
        QCOMPARE(phaseCount(parsed(o), 3), 1u);
    }

    void phaseCountWhenIdleFollowsModeAndWiring()
    {
        QJsonObject o = baseFields();
        o.insert("ChargeState", 1);
        o.insert("ChargePower", 0.0);
        for (int i = 1; i <= 3; ++i)
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);

        o.insert("ChargeMode", 1);
        QCOMPARE(phaseCount(parsed(o), 3), 3u);
        QCOMPARE(phaseCount(parsed(o), 1), 1u);
        o.insert("ChargeMode", 0);
        QCOMPARE(phaseCount(parsed(o), 3), 1u);
        o.insert("ChargeMode", 2);
        QCOMPARE(phaseCount(parsed(o), 3), 3u);
        o.remove("ChargeMode");
        QCOMPARE(phaseCount(parsed(o), 3), 3u);
        QCOMPARE(phaseCount(parsed(o), 1), 1u);
    }

    void phaseCountDuringAPausedSwitchIsNotMeasured()
    {
        // Paused, then switched to single phase: no current flows, so the mode decides.
        QJsonObject o = baseFields();
        o.insert("Paused", 1);
        o.insert("ChargePower", 0.0);
        o.insert("ChargeMode", 0);
        for (int i = 1; i <= 3; ++i)
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);
        QCOMPARE(phaseCount(parsed(o), 3), 1u);
    }

    void phaseCountWithoutMeasurements()
    {
        QJsonObject o = baseFields();
        for (int i = 1; i <= 3; ++i) {
            o.remove(QStringLiteral("IntensityMeasure_L%1").arg(i));
            o.remove(QStringLiteral("VoltageMeasure_L%1").arg(i));
        }
        QCOMPARE(phaseCount(parsed(o), 3), 3u);
        o.insert("ChargeMode", 0);
        QCOMPARE(phaseCount(parsed(o), 3), 1u);

        // Present but all-zero voltages: not populated, same as absent.
        QJsonObject z = baseFields();
        for (int i = 1; i <= 3; ++i)
            z.insert(QStringLiteral("VoltageMeasure_L%1").arg(i), 0.0);
        QVERIFY(!phaseMeasurementsAvailable(parsed(z)));
    }

    void singlePhaseWiringNeverAnnouncesThree()
    {
        QJsonObject o = baseFields();
        for (int mode : {0, 1, 2}) {
            o.insert("ChargeMode", mode);
            const DesiredPhases d = desiredPhases(parsed(o), 1);
            QCOMPARE(d.possibleValues, (QVariantList{1u}));
            QCOMPARE(*d.value, 1u);
            QVERIFY(!d.canSwitch);
        }
        o.remove("ChargeMode");
        QCOMPARE(desiredPhases(parsed(o), 1).possibleValues, (QVariantList{1u}));
    }

    void threePhaseWiringReadsChargeMode()
    {
        QJsonObject o = baseFields();
        o.insert("ChargeMode", 0);
        DesiredPhases d = desiredPhases(parsed(o), 3);
        QCOMPARE(d.possibleValues, (QVariantList{1u, 3u}));
        QCOMPARE(*d.value, 1u);
        QVERIFY(d.canSwitch);

        o.insert("ChargeMode", 1);
        QCOMPARE(*desiredPhases(parsed(o), 3).value, 3u);

        // Mixed: no fixed value to publish, switching stays possible.
        o.insert("ChargeMode", 2);
        d = desiredPhases(parsed(o), 3);
        QVERIFY(!d.value);
        QVERIFY(d.canSwitch);
        QCOMPARE(d.possibleValues, (QVariantList{1u, 3u}));
    }

    void firmwareWithoutChargeModeCannotSwitch()
    {
        QJsonObject o = baseFields();
        o.remove("ChargeMode");
        const DesiredPhases d = desiredPhases(parsed(o), 3);
        QCOMPARE(d.possibleValues, (QVariantList{3u}));
        QCOMPARE(*d.value, 3u);
        QVERIFY(!d.canSwitch);
        // An empty reading (before the first answer) gives the same: nothing announced.
        QCOMPARE(desiredPhases(TrydanRealTimeData(), 3).possibleValues, (QVariantList{3u}));
    }

    void chargeModeForPhases()
    {
        QCOMPARE(chargeModeFor(1), 0);
        QCOMPARE(chargeModeFor(3), 1);
    }

    void currentRangeFromTheCharger()
    {
        QJsonObject o = baseFields();
        CurrentRange r = currentRange(parsed(o));
        QCOMPARE(r.min, 6);
        QCOMPARE(r.max, 20);

        // Stricter than IEC 61851: the charger wins.
        o.insert("MinIntensity", 8);
        QCOMPARE(currentRange(parsed(o)).min, 8);

        // Looser: never below 6 A.
        o.insert("MinIntensity", 0);
        QCOMPARE(currentRange(parsed(o)).min, 6);

        // Inverted: closes on a point.
        o.insert("MinIntensity", 10);
        o.insert("MaxIntensity", 8);
        r = currentRange(parsed(o));
        QCOMPARE(r.min, 10);
        QCOMPARE(r.max, 10);

        // Nothing read yet: 6 A, the only value that is never wrong.
        r = currentRange(TrydanRealTimeData());
        QCOMPARE(r.min, 6);
        QCOMPARE(r.max, 6);
    }

    void setpointBelowTheMinimumPauses()
    {
        QJsonObject o = baseFields(); // charging, Intensity 6, range 6..20
        o.insert("Intensity", 10);
        CurrentCommand c = currentCommand(parsed(o), 5);
        QCOMPARE(c.kind, CurrentCommand::Pause);
        // Never the minimum instead, even just below it.
        QCOMPARE(currentCommand(parsed(o), 5.9).kind, CurrentCommand::Pause);
        QCOMPARE(currentCommand(parsed(o), 0).kind, CurrentCommand::Pause);
        QCOMPARE(currentCommand(parsed(o), qQNaN()).kind, CurrentCommand::Pause);

        // The minimum of the charger, when stricter than 6 A.
        o.insert("MinIntensity", 8);
        QCOMPARE(currentCommand(parsed(o), 7).kind, CurrentCommand::Pause);
        c = currentCommand(parsed(o), 8);
        QCOMPARE(c.kind, CurrentCommand::SetIntensity);
        QCOMPARE(c.ampere, 8);

        // Already paused: nothing to write.
        o.insert("Paused", 1);
        QCOMPARE(currentCommand(parsed(o), 5).kind, CurrentCommand::Nothing);
    }

    void setpointFromTheMinimumUpKeepsThePause()
    {
        QJsonObject o = baseFields();
        o.insert("Paused", 1);
        o.insert("Intensity", 6);
        // Written, the pause untouched: resuming is the power gesture.
        CurrentCommand c = currentCommand(parsed(o), 10);
        QCOMPARE(c.kind, CurrentCommand::SetIntensity);
        QCOMPARE(c.ampere, 10);

        // Rounded down: never more than asked.
        QCOMPARE(currentCommand(parsed(o), 10.7).ampere, 10);
        // Capped at the maximum of the charger.
        QCOMPARE(currentCommand(parsed(o), 32).ampere, 20);
        // Already there.
        QCOMPARE(currentCommand(parsed(o), 6).kind, CurrentCommand::Nothing);
    }

    // The choices offered to the installer are those of the metadata: read them there, so
    // that a label edited in the JSON cannot silently stop being recognised.
    void wiringChoicesOfTheMetadata()
    {
        QFile file(QStringLiteral(TRYDAN_PLUGIN_JSON));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject thingClass = QJsonDocument::fromJson(file.readAll()).object()
                .value("vendors").toArray().at(0).toObject()
                .value("thingClasses").toArray().at(0).toObject();
        QJsonObject wiring;
        for (const QJsonValue &p : thingClass.value("paramTypes").toArray()) {
            if (p.toObject().value("name").toString() == QLatin1String("wiring"))
                wiring = p.toObject();
        }
        QVERIFY(!wiring.isEmpty());
        // No default: nymea refuses a setup without it.
        QVERIFY(!wiring.contains("defaultValue"));
        const QJsonArray choices = wiring.value("allowedValues").toArray();
        QCOMPARE(choices.count(), 3);
        // The first one, which the app shows selected, is not a wiring.
        QVERIFY(!wiringFromParam(choices.at(0).toString()));
        QCOMPARE(*wiringFromParam(choices.at(1).toString()), 1u);
        QCOMPARE(*wiringFromParam(choices.at(2).toString()), 3u);

        // A charger added before the parameter existed.
        QVERIFY(!wiringFromParam(QString()));
        QVERIFY(!wiringFromParam(QStringLiteral("1")));
        // Nor did a setting of the old name survive as the wiring.
        for (const QJsonValue &s : thingClass.value("settingsTypes").toArray())
            QVERIFY(s.toObject().value("name").toString() != QLatin1String("phaseCount"));

        // The verdicts published are the possible values of the state.
        QJsonArray verdicts;
        for (const QJsonValue &s : thingClass.value("stateTypes").toArray()) {
            if (s.toObject().value("name").toString() == QLatin1String("wiringCheck"))
                verdicts = s.toObject().value("possibleValues").toArray();
        }
        QCOMPARE(verdicts, (QJsonArray{wiringCheckName(WiringCheck::NotChecked),
                                       wiringCheckName(WiringCheck::Consistent),
                                       wiringCheckName(WiringCheck::VoltageOnL2L3),
                                       wiringCheckName(WiringCheck::NoVoltageOnL2L3)}));
    }

    void wiringCheckFromTheVoltages()
    {
        QJsonObject o = baseFields(); // 230 V on L1..L3
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::Consistent);
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::VoltageOnL2L3);

        o.insert("VoltageMeasure_L2", 0.0);
        o.insert("VoltageMeasure_L3", 0.0);
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::Consistent);
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::NoVoltageOnL2L3);

        // One phase missing out of three is enough.
        o.insert("VoltageMeasure_L2", 231.0);
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::NoVoltageOnL2L3);
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::VoltageOnL2L3);

        // In between: no verdict.
        o.insert("VoltageMeasure_L2", 0.0);
        o.insert("VoltageMeasure_L3", 90.0);
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::NotChecked);

        // L1 not at mains voltage: the readings are not trusted.
        o = baseFields();
        o.insert("VoltageMeasure_L1", 0.0);
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::NotChecked);

        // Firmware without per-phase fields.
        o = baseFields();
        o.remove("VoltageMeasure_L2");
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::NotChecked);
        QCOMPARE(wiringCheckName(WiringCheck::NotChecked), QStringLiteral("Not checked"));
    }

    // Charging in single-phase mode on a three-phase supply (on-site test): the charger
    // itself leaves L2/L3 open, so they read no voltage. That is the mode chosen, not the
    // wiring: no verdict on an absence. A presence still confirms three phases.
    void wiringCheckFollowsThePhaseMode()
    {
        QJsonObject o = baseFields();
        o.insert("ChargeMode", 0);
        o.insert("ChargePower", 3350.42);
        o.insert("IntensityMeasure_L1", 15.02);
        o.insert("IntensityMeasure_L2", 0.0);
        o.insert("IntensityMeasure_L3", 0.0);
        o.insert("VoltageMeasure_L2", 0.0);
        o.insert("VoltageMeasure_L3", 0.0);
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::NotChecked);
        // Mixed: the charger picks the phases, L2/L3 may be open for the same reason.
        o.insert("ChargeMode", 2);
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::NotChecked);
        // Three phase mode, or a firmware that cannot switch: the check is back.
        o.insert("ChargeMode", 1);
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::NoVoltageOnL2L3);
        o.remove("ChargeMode");
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::NoVoltageOnL2L3);

        // Single-phase mode with voltage on L2/L3: three phases are there.
        o.insert("ChargeMode", 0);
        o.insert("VoltageMeasure_L2", 231.0);
        o.insert("VoltageMeasure_L3", 229.0);
        QCOMPARE(wiringCheck(parsed(o), 3), WiringCheck::Consistent);
        // Set as single phase, voltage on L2/L3: a mismatch in every mode.
        for (int mode : {0, 1, 2}) {
            o.insert("ChargeMode", mode);
            QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::VoltageOnL2L3);
        }
        // Set as single phase, single-phase mode, no voltage on L2/L3: consistent.
        o.insert("ChargeMode", 0);
        o.insert("VoltageMeasure_L2", 0.0);
        o.insert("VoltageMeasure_L3", 0.0);
        QCOMPARE(wiringCheck(parsed(o), 1), WiringCheck::Consistent);
    }
};

QTEST_GUILESS_MAIN(TestTrydanModel)
#include "tst_trydanmodel.moc"
