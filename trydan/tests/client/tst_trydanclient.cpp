// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HTTP client against a local fake Trydan (tests/common/faketrydan.*): the gestures
// the energy manager chains, their read-back, the faults, the polling bounds.

#include "faketrydan.h"
#include "trydanclient.h"
#include "trydanrealtimedata.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <memory>

namespace {

struct Outcome {
    bool finished = false;
    TrydanReply::Error error = TrydanReply::NoError;
    QString text;
    QString written;
};

// Waits for a reply, reading it inside the finished() slot: the client deletes it later.
Outcome await(TrydanReply *reply, int timeoutMs = 5000)
{
    auto outcome = std::make_shared<Outcome>();
    QObject::connect(reply, &TrydanReply::finished, reply, [outcome, reply]() {
        outcome->finished = true;
        outcome->error = reply->error();
        outcome->text = reply->errorString();
        outcome->written = reply->written();
    });
    (void)QTest::qWaitFor([outcome]() { return outcome->finished; }, timeoutMs);
    return *outcome;
}

}

class TestTrydanClient : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<FakeTrydan> m_charger;
    std::unique_ptr<TrydanClient> m_client;

    void charging3p(int amps)
    {
        m_charger->fields.insert("ChargeState", 2);
        m_charger->fields.insert("Paused", 0);
        m_charger->fields.insert("ChargeMode", 1);
        m_charger->fields.insert("Intensity", amps);
    }

private slots:
    void init()
    {
        m_charger = std::make_unique<FakeTrydan>();
        QVERIFY(m_charger->listen());
        m_client = std::make_unique<TrydanClient>(QStringLiteral("127.0.0.1"), m_charger->port());
        m_client->setRequestTimeout(500);
        m_client->setVerify(2, 50);
    }

    void cleanup()
    {
        m_client.reset();
        m_charger.reset();
    }

    void readsRealTimeData()
    {
        QVERIFY(!m_client->reachable());
        charging3p(10);
        const Outcome o = await(m_client->refresh());
        QVERIFY(o.finished);
        QCOMPARE(o.error, TrydanReply::NoError);
        QVERIFY(m_client->reachable());
        QVERIFY(m_client->data().looksLikeTrydan());
        QCOMPARE(*m_client->data().intensity, 10);
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 3u);
        QVERIFY(m_client->dataAgeMs() >= 0);
    }

    void switchAcceptedAndReadBack()
    {
        m_charger->fields.insert("ChargeMode", 1);
        const Outcome o = await(m_client->write("ChargeMode", 0));
        QCOMPARE(o.error, TrydanReply::NoError);
        QCOMPARE(m_charger->requests, (QStringList{"/write/ChargeMode=0", "/RealTimeData"}));
        QCOMPARE(m_charger->fields.value("ChargeMode").toInt(), 0);
        QCOMPARE(*m_client->data().chargeMode, 0);
        QCOMPARE(*TrydanModel::desiredPhases(m_client->data(), 3).value, 1u);
    }

    void writeRefused()
    {
        m_charger->writeReply = QStringLiteral("ERROR");
        await(m_client->refresh());
        const Outcome o = await(m_client->write("ChargeMode", 0));
        QCOMPARE(o.error, TrydanReply::Rejected);
        QVERIFY(o.text.contains("ERROR"));
        // A refusal is an answer: the link is fine, and nothing is read back.
        QVERIFY(m_client->reachable());
        QCOMPARE(m_client->consecutiveFailures(), 0);
        QCOMPARE(m_charger->requests.last(), QStringLiteral("/write/ChargeMode=0"));
        QCOMPARE(m_charger->fields.value("ChargeMode").toInt(), 1);
    }

    void requestTimesOut()
    {
        m_charger->silent = true;
        QElapsedTimer clock;
        clock.start();
        const Outcome o = await(m_client->write("ChargeMode", 0), 3000);
        QVERIFY(o.finished);
        QCOMPARE(o.error, TrydanReply::NetworkError);
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(m_client->consecutiveFailures(), 1);
    }

    void chargeModeNotTakenAtReadBack()
    {
        m_charger->ignoredParams.insert("ChargeMode");
        const Outcome o = await(m_client->write("ChargeMode", 0));
        QCOMPARE(o.error, TrydanReply::NotTaken);
        QVERIFY(o.text.contains("ChargeMode reads 1"));
        // One write, two readings (verify attempts), nothing more.
        QCOMPARE(m_charger->requests, (QStringList{"/write/ChargeMode=0", "/RealTimeData", "/RealTimeData"}));
    }

    void takenOneReadingLate()
    {
        m_charger->applyAfterReads = 2;
        const Outcome o = await(m_client->write("ChargeMode", 0));
        QCOMPARE(o.error, TrydanReply::NoError);
        QCOMPARE(m_charger->requests.count("/RealTimeData"), 2);
    }

    void firmwareWithoutChargeMode()
    {
        m_charger->sendChargeMode = false;
        const Outcome o = await(m_client->write("ChargeMode", 0));
        QCOMPARE(o.error, TrydanReply::Unsupported);
        QVERIFY(!TrydanModel::desiredPhases(m_client->data(), 3).canSwitch);
    }

    void disconnectionAndRecovery()
    {
        m_client->setPollInterval(30);
        QSignalSpy reachable(m_client.get(), &TrydanClient::reachableChanged);
        m_client->startPolling();
        QTRY_VERIFY(m_client->reachable());

        const quint16 port = m_charger->port();
        m_charger->shutdown();
        QTRY_VERIFY_WITH_TIMEOUT(!m_client->reachable(), 5000);
        QVERIFY(m_client->consecutiveFailures() >= TrydanClient::DefaultErrorLimit);
        QCOMPARE(reachable.count(), 2); // true, then false - once

        // Actions are refused by the plugin while unreachable; polling keeps trying.
        QVERIFY(m_charger->listen(port));
        QTRY_VERIFY_WITH_TIMEOUT(m_client->reachable(), 5000);
        QCOMPARE(m_client->consecutiveFailures(), 0);
        QCOMPARE(reachable.count(), 3);
    }

    void fewerFailuresThanTheLimitKeepTheChargerReachable()
    {
        await(m_client->refresh());
        m_charger->silent = true;
        await(m_client->refresh(), 3000);
        await(m_client->refresh(), 3000);
        QVERIFY(m_client->reachable());
        m_charger->silent = false;
        QCOMPARE(await(m_client->refresh()).error, TrydanReply::NoError);
        QCOMPARE(m_client->consecutiveFailures(), 0);
    }

    void pausedChargerStaysInChargeStateCharging()
    {
        charging3p(16);
        await(m_client->refresh());
        QVERIFY(TrydanModel::charging(m_client->data()));

        QCOMPARE(await(m_client->write("Paused", 1)).error, TrydanReply::NoError);
        const TrydanRealTimeData &d = m_client->data();
        QCOMPARE(*d.chargeState, 2);
        QCOMPARE(*d.chargePower, 0.0);
        QVERIFY(!TrydanModel::charging(d));
        QVERIFY(TrydanModel::pluggedIn(d, false));
        QCOMPARE(TrydanModel::phaseCurrents(d), (QList<double>{0, 0, 0}));
    }

    void mixedMode()
    {
        charging3p(10);
        m_charger->fields.insert("ChargeMode", 2);
        m_charger->mixedPhases = 1;
        await(m_client->refresh());
        QVERIFY(!TrydanModel::desiredPhases(m_client->data(), 3).value);
        QCOMPARE(TrydanModel::chargeMode(m_client->data()), TrydanModel::ChargeMode::Mixed);
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 1u);

        // The charger moves to three phases on its own.
        m_charger->mixedPhases = 3;
        await(m_client->refresh());
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 3u);

        // A command leaves mixed mode for a fixed one.
        QCOMPARE(await(m_client->write("ChargeMode", 0)).error, TrydanReply::NoError);
        QCOMPARE(*TrydanModel::desiredPhases(m_client->data(), 3).value, 1u);
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 1u);
    }

    void requestsNeverOverlap()
    {
        // The energy manager sends phase count, current and power in the same instant.
        m_charger->replyDelayMs = 30;
        TrydanReply *phases = m_client->write("ChargeMode", 0);
        TrydanReply *current = m_client->write("Intensity", 6);
        TrydanReply *power = m_client->write("Paused", 1);
        TrydanReply *read1 = m_client->refresh();
        TrydanReply *read2 = m_client->refresh();
        QCOMPARE(m_client->pending(), 4); // the two reads are one

        QCOMPARE(await(phases).error, TrydanReply::NoError);
        QCOMPARE(await(current).error, TrydanReply::NoError);
        QCOMPARE(await(power).error, TrydanReply::NoError);
        QCOMPARE(await(read1).error, TrydanReply::NoError);
        QCOMPARE(await(read2).error, TrydanReply::NoError);

        QCOMPARE(m_charger->maxConcurrent, 1);
        QCOMPARE(m_charger->requests, (QStringList{
            "/write/ChargeMode=0", "/RealTimeData",
            "/write/Intensity=6", "/RealTimeData",
            "/write/Paused=1", "/RealTimeData",
            "/RealTimeData"}));
    }

    void pollsDoNotPileUp()
    {
        m_charger->replyDelayMs = 200;
        m_client->setPollInterval(10);
        m_client->startPolling();
        QTest::qWait(1000);
        m_client->stopPolling();
        // 1 s of 200 ms replies: at most 5 requests, however fast the timer ticks.
        QVERIFY2(m_charger->requests.count() <= 6, qPrintable(QString::number(m_charger->requests.count())));
        QVERIFY(m_charger->requests.count() >= 3);
        QCOMPARE(m_charger->maxConcurrent, 1);
    }

    void abandonedWriteIsNotSent()
    {
        m_charger->replyDelayMs = 100;
        TrydanReply *first = m_client->write("Intensity", 8);
        TrydanReply *second = m_client->write("Intensity", 10);
        second->abandon();
        QCOMPARE(await(first).error, TrydanReply::NoError);
        QCOMPARE(await(second).error, TrydanReply::Aborted);
        QCOMPARE(m_charger->writesReceived("Intensity"), 1);
        QCOMPARE(m_charger->fields.value("Intensity").toInt(), 8);
    }

    void destroyedClientAbortsWhatIsPending()
    {
        m_charger->silent = true;
        auto outcome = std::make_shared<Outcome>();
        TrydanReply *reply = m_client->write("Paused", 1);
        connect(reply, &TrydanReply::finished, this, [outcome, reply]() {
            outcome->finished = true;
            outcome->error = reply->error();
        });
        QTest::qWait(50);
        m_client.reset();
        QVERIFY(outcome->finished);
        QCOMPARE(outcome->error, TrydanReply::Aborted);
    }

    // The sequence the energy manager will chain to go from three phases to one:
    // down to 6 A, pause, switch, resume. Each step is one gesture, read back.
    void engineSequenceThreeToOnePhase()
    {
        charging3p(16);
        await(m_client->refresh());
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 3u);

        // 1. Down to 6 A.
        QCOMPARE(await(m_client->write("Intensity", 6)).error, TrydanReply::NoError);
        QVERIFY(TrydanModel::charging(m_client->data()));
        QCOMPARE(*m_client->data().chargePower, 3 * 5.9 * 230.0);

        // 2. Pause: nothing flows, ChargeState still says charging.
        QCOMPARE(await(m_client->write("Paused", 1)).error, TrydanReply::NoError);
        QVERIFY(!TrydanModel::charging(m_client->data()));
        QCOMPARE(*m_client->data().chargeState, 2);

        // 3. Switch: taken, and read back. Not charging, so the mode gives phaseCount.
        QCOMPARE(await(m_client->write("ChargeMode", 0)).error, TrydanReply::NoError);
        QCOMPARE(*TrydanModel::desiredPhases(m_client->data(), 3).value, 1u);
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 1u);

        // 4. Resume: one phase drawn, the two others exactly 0.
        QCOMPARE(await(m_client->write("Paused", 0)).error, TrydanReply::NoError);
        const TrydanRealTimeData &d = m_client->data();
        QVERIFY(TrydanModel::charging(d));
        QCOMPARE(TrydanModel::phaseCount(d, 3), 1u);
        QCOMPARE(TrydanModel::phaseCurrents(d), (QList<double>{5.9, 0, 0}));
        QCOMPARE(*d.chargePower, 5.9 * 230.0);

        QCOMPARE(m_charger->requests, (QStringList{
            "/RealTimeData",
            "/write/Intensity=6", "/RealTimeData",
            "/write/Paused=1", "/RealTimeData",
            "/write/ChargeMode=0", "/RealTimeData",
            "/write/Paused=0", "/RealTimeData"}));
    }

    void engineSequenceOneToThreePhases()
    {
        m_charger->fields.insert("ChargeState", 2);
        m_charger->fields.insert("ChargeMode", 0);
        m_charger->fields.insert("Intensity", 16);
        await(m_client->refresh());
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 1u);

        QCOMPARE(await(m_client->write("Intensity", 6)).error, TrydanReply::NoError);
        QCOMPARE(await(m_client->write("Paused", 1)).error, TrydanReply::NoError);
        QCOMPARE(await(m_client->write("ChargeMode", 1)).error, TrydanReply::NoError);
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 3u);
        QCOMPARE(await(m_client->write("Paused", 0)).error, TrydanReply::NoError);
        QCOMPARE(TrydanModel::phaseCount(m_client->data(), 3), 3u);
        QCOMPARE(TrydanModel::phaseCurrents(m_client->data()), (QList<double>{5.9, 5.9, 5.9}));
    }

    void unchangedValueIsNotWritten()
    {
        await(m_client->refresh());
        const Outcome o = await(m_client->write("Intensity", 16));
        QCOMPARE(o.error, TrydanReply::NoError);
        QVERIFY(o.written.isEmpty());
        QCOMPARE(m_charger->requests, (QStringList{"/RealTimeData"}));
    }

    // Below the minimum, the charger does not work: charging stops (pause), it is never
    // set to the minimum instead.
    void setpointBelowTheMinimumWhileChargingPauses()
    {
        charging3p(16);
        await(m_client->refresh());
        QVERIFY(TrydanModel::charging(m_client->data()));

        const Outcome o = await(m_client->setChargingCurrent(5));
        QCOMPARE(o.error, TrydanReply::NoError);
        QCOMPARE(o.written, QStringLiteral("Paused=1"));
        // Written, then read back.
        QCOMPARE(m_charger->requests, (QStringList{"/RealTimeData", "/write/Paused=1", "/RealTimeData"}));
        const TrydanRealTimeData &d = m_client->data();
        QCOMPARE(*d.paused, 1);
        QVERIFY(!TrydanModel::charging(d));
        QCOMPARE(*d.chargePower, 0.0);
        // The setpoint is not touched.
        QCOMPARE(*d.intensity, 16);
        QCOMPARE(m_charger->writesReceived("Intensity"), 0);
    }

    void setpointBelowTheMinimumWhenStoppedStartsNothing()
    {
        // A vehicle plugged in, the charger paused.
        charging3p(16);
        m_charger->fields.insert("Paused", 1);
        await(m_client->refresh());

        const Outcome o = await(m_client->setChargingCurrent(5));
        QCOMPARE(o.error, TrydanReply::NoError);
        QVERIFY(o.written.isEmpty());
        QCOMPARE(m_charger->requests, (QStringList{"/RealTimeData"}));
        QCOMPARE(await(m_client->refresh()).error, TrydanReply::NoError);
        QCOMPARE(*m_client->data().paused, 1);
        QVERIFY(!TrydanModel::charging(m_client->data()));
        QCOMPARE(*m_client->data().intensity, 16);
    }

    void setpointBackAboveTheMinimumKeepsThePause()
    {
        charging3p(16);
        await(m_client->refresh());
        QCOMPARE(await(m_client->setChargingCurrent(5)).written, QStringLiteral("Paused=1"));

        const Outcome o = await(m_client->setChargingCurrent(10));
        QCOMPARE(o.error, TrydanReply::NoError);
        QCOMPARE(o.written, QStringLiteral("Intensity=10"));
        const TrydanRealTimeData &d = m_client->data();
        QCOMPARE(*d.intensity, 10);
        // Still paused: resuming is the power gesture.
        QCOMPARE(*d.paused, 1);
        QVERIFY(!TrydanModel::charging(d));
        QCOMPARE(m_charger->writesReceived("Paused"), 1);
        QCOMPARE(m_charger->requests.mid(3), (QStringList{"/write/Intensity=10", "/RealTimeData"}));

        // The engine resumes: at 10 A.
        QCOMPARE(await(m_client->write("Paused", 0)).error, TrydanReply::NoError);
        QCOMPARE(*m_client->data().chargePower, 3 * 9.9 * 230.0);
    }

    void setpointAtOrAboveTheMinimumRoundsDownAndCaps()
    {
        charging3p(16);
        m_charger->fields.insert("MaxIntensity", 20);
        await(m_client->refresh());
        QCOMPARE(await(m_client->setChargingCurrent(10.7)).written, QStringLiteral("Intensity=10"));
        QCOMPARE(await(m_client->setChargingCurrent(32)).written, QStringLiteral("Intensity=20"));
        QCOMPARE(m_charger->writesReceived("Paused"), 0);
    }

    // The energy manager sends its gestures in the same instant. Each one is decided on
    // what the previous one left, not on the reading at the time it was queued.
    void setpointDecidedWhenSentNotWhenQueued()
    {
        charging3p(16);
        m_charger->fields.insert("Paused", 1);
        await(m_client->refresh());

        TrydanReply *resume = m_client->write("Paused", 0);
        TrydanReply *current = m_client->setChargingCurrent(5);
        QCOMPARE(await(resume).written, QStringLiteral("Paused=0"));
        // Queued while the reading said "paused"; sent after the resume: pauses again.
        QCOMPARE(await(current).written, QStringLiteral("Paused=1"));
        QCOMPARE(*m_client->data().paused, 1);
        QVERIFY(!TrydanModel::charging(m_client->data()));
    }

    // The wiring set by the installer, against the voltages the charger reports. Read
    // only: the check writes nothing.
    void wiringCheckAgainstTheCharger()
    {
        using TrydanModel::WiringCheck;

        // Three-phase supply.
        await(m_client->refresh());
        QCOMPARE(TrydanModel::wiringCheck(m_client->data(), 3), WiringCheck::Consistent);
        QCOMPARE(TrydanModel::wiringCheck(m_client->data(), 1), WiringCheck::VoltageOnL2L3);

        // Single-phase supply.
        m_charger->fields.insert("VoltageMeasure_L2", 0.0);
        m_charger->fields.insert("VoltageMeasure_L3", 0.0);
        await(m_client->refresh());
        QCOMPARE(TrydanModel::wiringCheck(m_client->data(), 1), WiringCheck::Consistent);
        QCOMPARE(TrydanModel::wiringCheck(m_client->data(), 3), WiringCheck::NoVoltageOnL2L3);

        // Firmware without the per-phase fields: nothing to say.
        m_charger->sendPhaseFields = false;
        await(m_client->refresh());
        QCOMPARE(TrydanModel::wiringCheck(m_client->data(), 1), WiringCheck::NotChecked);
        QCOMPARE(TrydanModel::wiringCheck(m_client->data(), 3), WiringCheck::NotChecked);

        QCOMPARE(m_charger->requests, (QStringList{"/RealTimeData", "/RealTimeData", "/RealTimeData"}));
    }
};

QTEST_GUILESS_MAIN(TestTrydanClient)
#include "tst_trydanclient.moc"
