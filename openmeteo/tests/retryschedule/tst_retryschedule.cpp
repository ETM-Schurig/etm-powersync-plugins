// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of the retry schedule (CONTRACT_A sec. 3.1). The clock is a plain
// integer advanced by the test; the jitter is injected. No timer, no network.

#include "retryschedule.h"

#include <QTest>

namespace {

const QString Key = QStringLiteral("{site}/ensemble");
const int Ensemble = 10800;
const int Forecast = 3600;

// Plays the plugin's minute tick against a source that fails every time, for a given
// duration, and returns the instants of the attempts.
QList<qint64> attemptsOfAFailingSource(RetrySchedule &schedule, int intervalS, qint64 durationS,
                                       qint64 replyDelayS = 0)
{
    QList<qint64> attempts;
    for (qint64 now = 0; now <= durationS; now += 60) {
        if (!schedule.due(Key, now))
            continue;
        attempts.append(now);
        schedule.started(Key, intervalS, now);
        schedule.failed(Key, intervalS, now + replyDelayS);
    }
    return attempts;
}

}

class TestRetrySchedule : public QObject
{
    Q_OBJECT

private slots:
    void delaysDoubleUpToTheInterval();
    void aFailingSourceIsNoLongerAskedEveryMinute();
    void theDelayRunsFromTheFailure();
    void successResetsTheCount();
    void successSchedulesOneIntervalLater();
    void aReplyThatNeverComesStillDoubles();
    void aFailureWithoutAttemptCountsOne();
    void jitterIsBoundedAndApplied();
    void defaultJitterIsWithinSixtySeconds();
    void sourcesAreIndependent();
};

void TestRetrySchedule::delaysDoubleUpToTheInterval()
{
    // 5 min x 2^(n-1), capped at the interval of the source.
    const QList<int> forecast = {300, 600, 1200, 2400, 3600, 3600, 3600};
    for (int n = 1; n <= forecast.count(); n++)
        QCOMPARE(RetrySchedule::retryDelayS(n, Forecast), forecast.at(n - 1));
    const QList<int> ensemble = {300, 600, 1200, 2400, 4800, 9600, 10800, 10800};
    for (int n = 1; n <= ensemble.count(); n++)
        QCOMPARE(RetrySchedule::retryDelayS(n, Ensemble), ensemble.at(n - 1));
    QCOMPARE(RetrySchedule::retryDelayS(0, Ensemble), Ensemble);
    // No overflow on a source that failed for weeks.
    QCOMPARE(RetrySchedule::retryDelayS(5000, Ensemble), Ensemble);
}

void TestRetrySchedule::aFailingSourceIsNoLongerAskedEveryMinute()
{
    // The case observed in testing: the ensemble answers 500 for a day. etm15 asked
    // again at every tick, 1440 times. Now: 0, 5, 15, 35, 75, 155, 315 min, then every 3 h.
    RetrySchedule schedule([]() { return 0; });
    const QList<qint64> attempts = attemptsOfAFailingSource(schedule, Ensemble, 24 * 3600);
    const QList<qint64> expectedStart = {0, 300, 900, 2100, 4500, 9300, 18900, 29700};
    QCOMPARE(attempts.mid(0, expectedStart.count()), expectedStart);
    for (int i = expectedStart.count(); i < attempts.count(); i++)
        QCOMPARE(attempts.at(i) - attempts.at(i - 1), qint64(Ensemble));
    QCOMPARE(attempts.count(), 13);

    RetrySchedule forecast([]() { return 0; });
    const QList<qint64> f = attemptsOfAFailingSource(forecast, Forecast, 24 * 3600);
    QCOMPARE(f.mid(0, 6), (QList<qint64>{0, 300, 900, 2100, 4500, 8100}));
    QCOMPARE(f.count(), 27);   // 5 backed-off, then 22 hourly - against 1440
}

void TestRetrySchedule::theDelayRunsFromTheFailure()
{
    // A request that takes 2 min to fail is retried 5 min after the failure, not 3 min.
    RetrySchedule schedule([]() { return 0; });
    const QList<qint64> attempts = attemptsOfAFailingSource(schedule, Ensemble, 3600, 120);
    QCOMPARE(attempts.mid(0, 3), (QList<qint64>{0, 420, 1140}));
}

void TestRetrySchedule::successResetsTheCount()
{
    RetrySchedule schedule([]() { return 0; });
    qint64 now = 0;
    for (int i = 0; i < 4; i++) {
        schedule.started(Key, Ensemble, now);
        schedule.failed(Key, Ensemble, now);
        now = schedule.nextAttempt(Key);
    }
    QCOMPARE(schedule.failures(Key), 4);
    QCOMPARE(schedule.nextAttempt(Key) - now, qint64(0));
    schedule.started(Key, Ensemble, now);
    schedule.succeeded(Key, Ensemble, now + 2);
    QCOMPARE(schedule.failures(Key), 0);
    QCOMPARE(schedule.lastSuccess(Key), now + 2);
    // The next failure starts over at 5 min.
    const qint64 later = schedule.nextAttempt(Key);
    schedule.started(Key, Ensemble, later);
    schedule.failed(Key, Ensemble, later);
    QCOMPARE(schedule.nextAttempt(Key) - later, qint64(300));
}

void TestRetrySchedule::successSchedulesOneIntervalLater()
{
    RetrySchedule schedule([]() { return 0; });
    QVERIFY(schedule.due(Key, 0));          // never asked: due at once
    QCOMPARE(schedule.lastSuccess(Key), qint64(0));
    schedule.started(Key, Forecast, 0);
    QVERIFY(!schedule.due(Key, 60));        // in flight: not asked again at the next tick
    schedule.succeeded(Key, Forecast, 5);
    QVERIFY(!schedule.due(Key, 3604));
    QVERIFY(schedule.due(Key, 3605));
}

void TestRetrySchedule::aReplyThatNeverComesStillDoubles()
{
    // No reply, no failure reported: the attempt itself counted, so a request hanging
    // forever is asked again at 5, then 10, then 20 min - never every minute, never never.
    RetrySchedule schedule([]() { return 0; });
    QList<qint64> attempts;
    for (qint64 now = 0; now <= 3600; now += 60) {
        if (schedule.due(Key, now)) {
            attempts.append(now);
            schedule.started(Key, Ensemble, now);
        }
    }
    QCOMPARE(attempts, (QList<qint64>{0, 300, 900, 2100}));
}

void TestRetrySchedule::aFailureWithoutAttemptCountsOne()
{
    RetrySchedule schedule([]() { return 0; });
    schedule.failed(Key, Ensemble, 100);
    QCOMPARE(schedule.failures(Key), 1);
    QCOMPARE(schedule.nextAttempt(Key), qint64(400));
}

void TestRetrySchedule::jitterIsBoundedAndApplied()
{
    int next = 0;
    const QList<int> rolls = {45, -60, 999, -999};
    RetrySchedule schedule([&]() { return rolls.at(next++); });
    schedule.started(Key, Ensemble, 0);
    QCOMPARE(schedule.nextAttempt(Key), qint64(300 + 45));
    schedule.failed(Key, Ensemble, 0);
    QCOMPARE(schedule.nextAttempt(Key), qint64(300 - 60));
    schedule.succeeded(Key, Ensemble, 0);
    QCOMPARE(schedule.nextAttempt(Key), qint64(Ensemble + 60));   // clamped to +60
    schedule.started(Key, Ensemble, 0);
    QCOMPARE(schedule.nextAttempt(Key), qint64(300 - 60));        // clamped to -60
}

void TestRetrySchedule::defaultJitterIsWithinSixtySeconds()
{
    RetrySchedule schedule;
    qint64 low = 1000, high = -1000;
    for (int i = 0; i < 2000; i++) {
        schedule.succeeded(Key, Forecast, 0);
        const qint64 j = schedule.nextAttempt(Key) - Forecast;
        low = qMin(low, j);
        high = qMax(high, j);
    }
    QVERIFY(low >= -60 && high <= 60);
    // And it does spread: 2000 rolls over 121 values reach both ends' neighbourhood.
    QVERIFY(low <= -50 && high >= 50);
}

void TestRetrySchedule::sourcesAreIndependent()
{
    RetrySchedule schedule([]() { return 0; });
    const QString other = QStringLiteral("{site}/satellite");
    schedule.started(Key, Ensemble, 0);
    schedule.failed(Key, Ensemble, 0);
    QVERIFY(schedule.due(other, 0));
    QCOMPARE(schedule.failures(other), 0);
    schedule.remove(Key);
    QVERIFY(schedule.due(Key, 0));
    QCOMPARE(schedule.failures(Key), 0);
}

QTEST_APPLESS_MAIN(TestRetrySchedule)
#include "tst_retryschedule.moc"
