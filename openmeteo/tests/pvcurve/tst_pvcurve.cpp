// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of the forecastCurveTransport builder (CONTRACT_A sec. 5).
//
// Every fixture under ../fixtures is a reply recorded from api.open-meteo.com with the
// request the plugin sends for a plane (tilt 30, azimuth 0 Open-Meteo = South, hourly
// global_tilted_irradiance,temperature_2m). No test opens a socket. See ../fixtures/README.md.
//
// The reference values are computed by hand, not by the code under test: an identity
// whose two terms share one source verifies nothing (CONTRACT_A sec. 2.2).

#include "pvcurve.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QTest>
#include <QtMath>

namespace {

QVariantMap loadFixture(const QString &name)
{
    QFile file(QStringLiteral(FIXTURES_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("fixture %s not readable", qPrintable(name));
    return QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
}

uint localDayStart(uint unixTime, int utcOffsetS)
{
    const qint64 shifted = static_cast<qint64>(unixTime) + utcOffsetS;
    return static_cast<uint>((shifted / 86400) * 86400 - utcOffsetS);
}

// The v1 loop of 1.15.2+etm14 (integrationpluginopenmeteo.cpp:835-851), frozen here as
// the non-regression reference for an uncorrected v2 curve.
QVariantList v1Points(const QVariantList &times, const QVariantList &gtis, uint todayStart,
                      double kwp, double pr)
{
    QVariantList points;
    for (int slot = 0; slot < 48; slot++) {
        const uint sampleTime = todayStart + (slot + 1) * 3600;
        int index = -1;
        for (int i = 0; i < times.count(); i++) {
            if (times.at(i).toUInt() == sampleTime) {
                index = i;
                break;
            }
        }
        if (index < 0 || gtis.at(index).isNull())
            return QVariantList();
        points.append(qRound(gtis.at(index).toDouble() * kwp * pr));
    }
    return points;
}

// Instant -> value, built independently of the index walk in PvCurve::build.
QHash<uint, QVariant> byInstant(const QVariantList &times, const QVariantList &values)
{
    QHash<uint, QVariant> map;
    for (int i = 0; i < times.count(); i++)
        map.insert(times.at(i).toUInt(), values.at(i));
    return map;
}

PvCurve::Plane plane(double kwp = 6.0, double pr = 0.9)
{
    PvCurve::Plane p;
    p.kwp = kwp;
    p.performanceRatio = pr;
    return p;
}

// Three hourly instants from t0, with the given GTI and temperature, then enough flat
// hours to fill the 48-slot window.
void syntheticSeries(uint t0, const QList<double> &gti, const QList<QVariant> &temp,
                     QVariantList &times, QVariantList &g, QVariantList &t)
{
    for (int i = 0; i <= 48; i++) {
        times.append(t0 + i * 3600);
        const int k = i - 1;
        g.append(k >= 0 && k < gti.count() ? gti.at(k) : 0.0);
        t.append(k >= 0 && k < temp.count() ? temp.at(k) : QVariant(10.0));
    }
}

}

class TestPvCurve : public QObject
{
    Q_OBJECT

private slots:
    void referenceValues();
    void zeroIrradianceGivesZero();
    void powerIsFlooredAtZero();
    void realDayIsCorrected();
    void temperatureAbsentLeavesV1Untouched();
    void temperaturePartialLeavesV1Untouched();
    void temperatureOfAnotherLengthIsAbsent();
    void nullTemperatureOutsideTheWindowDoesNotMatter();
    void slotsAreReadByInstantNotByIndex();
    void day25hAlignsByInstant();
    void day23hAlignsByInstant();
    void missingGtiPublishesNothing();
    void meanGapOnARealDay();
    void transportIsBitIdenticalToEtm15();
    void scalarsSumThePublishedCurve();
    void scalarsFollowTheDecisionOfTheRun();
    void pvPowerIsLeftOutWithoutCurrentTemperature();
    void elevationEffectOnARealReply();
};

void TestPvCurve::referenceValues()
{
    // Hand computation, coefficient -0.37 %/K, NOCT 45 C:
    //   1000 W/m2, 25 C: Tcell = 25 + 1000*25/800 = 56.25 ; 1 - 0.0037*31.25 = 0.884375
    //    800 W/m2, -5 C: Tcell = -5 +  800*25/800 = 20    ; 1 - 0.0037*(-5)  = 1.0185
    QCOMPARE(PvCurve::cellTemperature(1000, 25, 45), 56.25);
    QVERIFY(qAbs(PvCurve::temperatureFactor(1000, 25, -0.37, 45) - 0.884375) < 1e-12);
    QCOMPARE(PvCurve::cellTemperature(800, -5, 45), 20.0);
    QVERIFY(qAbs(PvCurve::temperatureFactor(800, -5, -0.37, 45) - 1.0185) < 1e-12);

    // Through the payload: 1 kWp, PR 1. 1000 * 0.884375 = 884.375 -> 884 ;
    // 800 * 1.0185 = 814.8 -> 815, above the uncorrected 800 (a gain in the cold).
    const uint t0 = 1791842400;
    QVariantList times, g, t;
    syntheticSeries(t0, {1000, 800}, {25.0, -5.0}, times, g, t);
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, g, t, t0, plane(1.0, 1.0), curve));
    QCOMPARE(curve.value("corrections").toList(), QVariantList{QStringLiteral("temperature")});
    QCOMPARE(curve.value("p").toList().at(0).toInt(), 884);
    QCOMPARE(curve.value("p").toList().at(1).toInt(), 815);
}

void TestPvCurve::zeroIrradianceGivesZero()
{
    // At night Tcell = T2m and the factor is not 1, but nothing times it is still 0.
    QVERIFY(qAbs(PvCurve::temperatureFactor(0, -10, -0.37, 45) - 1.1295) < 1e-12);
    const uint t0 = 1791842400;
    QVariantList times, g, t;
    syntheticSeries(t0, {0, 0}, {-10.0, 40.0}, times, g, t);
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, g, t, t0, plane(), curve));
    foreach (const QVariant &p, curve.value("p").toList())
        QCOMPARE(p.toInt(), 0);
}

void TestPvCurve::powerIsFlooredAtZero()
{
    // An absurd but admitted pair (-1 %/K, NOCT 70) on a hot bright hour: Tcell =
    // 55 + 1200*50/800 = 130 C, factor 1 - 0.01*105 = -0.05. Floored, never negative.
    PvCurve::Plane hot = plane(1.0, 1.0);
    hot.tempCoeffPctPerK = -1.0;
    hot.noctC = 70;
    QVERIFY(PvCurve::temperatureFactor(1200, 55, -1.0, 70) < 0);
    const uint t0 = 1791842400;
    QVariantList times, g, t;
    syntheticSeries(t0, {1200}, {55.0}, times, g, t);
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, g, t, t0, hot, curve));
    QCOMPARE(curve.value("p").toList().at(0).toInt(), 0);
}

void TestPvCurve::realDayIsCorrected()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());

    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, temp, t0, plane(), curve));

    QCOMPARE(curve.value("v").toInt(), 2);
    QCOMPARE(curve.value("t0").toUInt(), t0);
    QCOMPARE(curve.value("step").toInt(), 3600);
    QCOMPARE(curve.value("unit").toString(), QStringLiteral("W"));
    QCOMPARE(curve.value("convention").toString(), QStringLiteral("mean over [t, t+step)"));
    QCOMPARE(curve.value("regime").toString(), QStringLiteral("forecast"));
    // ems refuses anything but a list here (MALFORMED), and the string "none" with it.
    QCOMPARE(curve.value("corrections").userType(), int(QMetaType::QVariantList));
    QCOMPARE(curve.value("corrections").toList(), QVariantList{QStringLiteral("temperature")});
    QCOMPARE(curve.value("p").toList().count(), 48);
    QCOMPARE(curve.value("gti").toList().count(), 48);
    QCOMPARE(curve.value("tempC").toList().count(), 48);

    const QHash<uint, QVariant> g = byInstant(times, gti);
    const QHash<uint, QVariant> t = byInstant(times, temp);
    for (int i = 0; i < 48; i++) {
        const uint at = t0 + (i + 1) * 3600;
        QCOMPARE(curve.value("gti").toList().at(i).toDouble(), g.value(at).toDouble());
        QCOMPARE(curve.value("tempC").toList().at(i).toDouble(), t.value(at).toDouble());
    }

    // One slot recomputed by hand from the fixture: the brightest of the day, 709.4 W/m2
    // at 16.9 C. Tcell = 16.9 + 709.4*25/800 = 39.06875 ; factor 1 - 0.0037*14.06875
    // = 0.947945625 ; 709.4 * 6 * 0.9 * 0.947945625 = 3631.35 -> 3631 (v1: 3831).
    int brightest = -1;
    for (int i = 0; i < 48; i++) {
        if (curve.value("gti").toList().at(i).toDouble() == 709.4)
            brightest = i;
    }
    QVERIFY(brightest >= 0);
    QCOMPARE(curve.value("tempC").toList().at(brightest).toDouble(), 16.9);
    QCOMPARE(curve.value("p").toList().at(brightest).toInt(), 3631);
}

void TestPvCurve::temperatureAbsentLeavesV1Untouched()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());

    // Odd parameters on purpose: the bit-for-bit claim must not rely on round numbers.
    const PvCurve::Plane odd = plane(7.37, 0.83);
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, QVariantList(), t0, odd, curve));
    QCOMPARE(curve.value("corrections").userType(), int(QMetaType::QVariantList));
    QVERIFY(curve.value("corrections").toList().isEmpty());
    QCOMPARE(curve.value("p").toList(), v1Points(times, gti, t0, 7.37, 0.83));
    QCOMPARE(curve.value("tempC").toList().count(), 48);
    foreach (const QVariant &c, curve.value("tempC").toList())
        QVERIFY(c.isNull());
}

void TestPvCurve::temperaturePartialLeavesV1Untouched()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());

    // One hole, at the brightest hour of tomorrow: the whole run goes uncorrected.
    const int hole = times.indexOf(QVariant(t0 + 38 * 3600));
    QVERIFY(hole >= 0);
    temp[hole] = QVariant();

    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, temp, t0, plane(), curve));
    QVERIFY(curve.value("corrections").toList().isEmpty());
    QCOMPARE(curve.value("p").toList(), v1Points(times, gti, t0, 6.0, 0.9));
    // What is known is still published for the record; what is not is null, never 0.
    QVERIFY(curve.value("tempC").toList().at(37).isNull());
    QCOMPARE(curve.value("tempC").toList().at(36).toDouble(), temp.at(hole - 1).toDouble());
}

void TestPvCurve::temperatureOfAnotherLengthIsAbsent()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList().mid(1);
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());

    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, temp, t0, plane(), curve));
    QVERIFY(curve.value("corrections").toList().isEmpty());
    QCOMPARE(curve.value("p").toList(), v1Points(times, gti, t0, 6.0, 0.9));
    foreach (const QVariant &c, curve.value("tempC").toList())
        QVERIFY(c.isNull());
}

void TestPvCurve::nullTemperatureOutsideTheWindowDoesNotMatter()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());

    // Index 0 is t0 itself (the mean of the hour BEFORE the window) and the last one is
    // day 3: neither is read.
    temp[0] = QVariant();
    temp[temp.count() - 1] = QVariant();
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, temp, t0, plane(), curve));
    QCOMPARE(curve.value("corrections").toList(), QVariantList{QStringLiteral("temperature")});
}

void TestPvCurve::slotsAreReadByInstantNotByIndex()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());

    // The reply starts AT t0, so slot i is index i+1. An index walk would be one hour off.
    QCOMPARE(times.at(0).toUInt(), t0);
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, temp, t0, plane(), curve));
    for (int i = 0; i < 48; i++) {
        QCOMPARE(curve.value("tempC").toList().at(i).toDouble(), temp.at(i + 1).toDouble());
        QCOMPARE(curve.value("gti").toList().at(i).toDouble(), gti.at(i + 1).toDouble());
    }
    // And the test can tell: on this reply an index walk reads different temperatures.
    int differ = 0;
    for (int i = 0; i < 48; i++)
        differ += temp.at(i) != temp.at(i + 1) ? 1 : 0;
    QVERIFY(differ > 24);
}

void TestPvCurve::day25hAlignsByInstant()
{
    // 2026-10-25, end of summer time in Europe/Paris: a 25 h local day. Recorded with
    // start_date=2026-10-25: Open-Meteo answers on ONE fixed offset for the whole range
    // (utc_offset_seconds 7200, 72 samples) - the API itself has no 25 h day.
    const QVariantMap reply = loadFixture("plane-best_match-dst-end-2026-10-25.json");
    QCOMPARE(reply.value("utc_offset_seconds").toInt(), 7200);
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList();
    const QHash<uint, QVariant> g = byInstant(times, gti);
    const QHash<uint, QVariant> t = byInstant(times, temp);

    // Local midnight read with the summer offset (22:00Z) and with the winter one
    // (23:00Z): the second is NOT on the reply's day grid. Both must pair GTI and
    // temperature at the same instant, slot after slot, with no gap at the change.
    const uint now = 1792929600; // 2026-10-25 12:00Z
    const uint midnights[] = { localDayStart(now, 7200), localDayStart(now, 3600) };
    QCOMPARE(midnights[0], 1792879200u);
    QCOMPARE(midnights[1], 1792882800u);
    for (const uint t0 : midnights) {
        QVariantMap curve;
        QVERIFY(PvCurve::build(times, gti, temp, t0, plane(), curve));
        QCOMPARE(curve.value("corrections").toList(), QVariantList{QStringLiteral("temperature")});
        for (int i = 0; i < 48; i++) {
            const uint at = t0 + (i + 1) * 3600;
            QCOMPARE(curve.value("gti").toList().at(i).toDouble(), g.value(at).toDouble());
            QCOMPARE(curve.value("tempC").toList().at(i).toDouble(), t.value(at).toDouble());
        }
    }
}

void TestPvCurve::day23hAlignsByInstant()
{
    // 2026-03-29, start of summer time: a 23 h local day. SYNTHETIC - the API serves no
    // date that far back (allowed range 2026-07-12 .. 2026-10-28 when recorded). Each
    // sample encodes its own instant, so any shift between the series shows.
    const uint base = 1774738800; // 2026-03-28 23:00Z = 2026-03-29 00:00 CET
    QVariantList times, gti, temp;
    for (int i = 0; i < 72; i++) {
        const uint at = base + i * 3600;
        times.append(at);
        gti.append(double((at / 3600) % 1000));
        temp.append(double((at / 3600) % 1000) / 100.0);
    }
    // The reply is on CET (+3600); the day is 23 h long, so tomorrow's midnight is
    // 22:00Z. The curve is 48 absolute hours from t0 whatever the local day length.
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, gti, temp, base, plane(1.0, 1.0), curve));
    for (int i = 0; i < 48; i++) {
        const uint at = base + (i + 1) * 3600;
        QCOMPARE(curve.value("gti").toList().at(i).toDouble(), double((at / 3600) % 1000));
        QCOMPARE(curve.value("tempC").toList().at(i).toDouble(), double((at / 3600) % 1000) / 100.0);
    }
}

void TestPvCurve::missingGtiPublishesNothing()
{
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());
    gti[20] = QVariant();
    QVariantMap curve;
    QVERIFY(!PvCurve::build(times, gti, temp, t0, plane(), curve));
    QVERIFY(curve.isEmpty());
}

void TestPvCurve::meanGapOnARealDay()
{
    // Not an assertion on physics: the order of magnitude of the correction on a real
    // reply, for the report. 2026-10-13, tilt 30 South, defaults.
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());
    QVariantMap v2, v1;
    QVERIFY(PvCurve::build(times, gti, temp, t0, plane(1.0, 1.0), v2));
    QVERIFY(PvCurve::build(times, gti, QVariantList(), t0, plane(1.0, 1.0), v1));
    for (int day = 0; day < 2; day++) {
        double e1 = 0, e2 = 0, sumAbs = 0, maxGap = 0;
        int lit = 0;
        for (int i = day * 24; i < (day + 1) * 24; i++) {
            const double p1 = v1.value("p").toList().at(i).toDouble();
            const double p2 = v2.value("p").toList().at(i).toDouble();
            e1 += p1;
            e2 += p2;
            if (p1 > 0) {
                lit++;
                sumAbs += qAbs(p2 - p1);
                maxGap = qMax(maxGap, qAbs(p2 - p1) / p1);
            }
        }
        qInfo("day %d (1 kWp, PR 1): v1 %.0f Wh, v2 %.0f Wh, energy gap %+.2f %%, "
              "mean |gap| %.1f W over %d lit hours, max hourly gap %.2f %%",
              day, e1, e2, 100.0 * (e2 - e1) / e1, sumAbs / lit, lit, 100.0 * maxGap);
    }
}

void TestPvCurve::transportIsBitIdenticalToEtm15()
{
    // expected-transport-etm15.json was written by the pvcurve.cpp of bfea4068 (etm15),
    // compiled apart, over four recorded replies and three planes, with and without the
    // temperature series. The hourly part of the curve must come out the same, byte for
    // byte once serialised: etm16 changes how the scalar states are computed, never the
    // transport (an ongoing data collection relies on it).
    QFile file(QStringLiteral(FIXTURES_DIR "/expected-transport-etm15.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QVariantList cases = QJsonDocument::fromJson(file.readAll()).toVariant().toList();
    QCOMPARE(cases.count(), 24);
    int built = 0;
    foreach (const QVariant &c, cases) {
        const QVariantMap expected = c.toMap();
        const QVariantMap reply = loadFixture(expected.value("fixture").toString());
        const QVariantMap hourly = reply.value("hourly").toMap();
        PvCurve::Plane p;
        p.kwp = expected.value("kwp").toDouble();
        p.performanceRatio = expected.value("pr").toDouble();
        p.tempCoeffPctPerK = expected.value("coeff").toDouble();
        p.noctC = expected.value("noct").toDouble();
        QVariantMap curve;
        const bool ok = PvCurve::build(hourly.value("time").toList(),
                                       hourly.value("global_tilted_irradiance").toList(),
                                       expected.value("withTemperature").toBool()
                                           ? hourly.value("temperature_2m").toList() : QVariantList(),
                                       expected.value("t0").toUInt(), p, curve);
        QCOMPARE(ok, expected.value("ok").toBool());
        QCOMPARE(QJsonDocument::fromVariant(curve).toJson(QJsonDocument::Compact),
                 QJsonDocument::fromVariant(expected.value("curve")).toJson(QJsonDocument::Compact));
        built += ok ? 1 : 0;
    }
    // The 2026-10-25 reply does not hold the window of the day it was recorded on.
    QCOMPARE(built, 18);
}

void TestPvCurve::scalarsSumThePublishedCurve()
{
    // Real reply of 2026-10-15 07:30 local, a test plane (2 kWp, PR 0.85, tilt 50
    // South). Every energy must be the sum of the published p over its window, found
    // here by instant and independently of PvCurve::scalars.
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-15-elevation153.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList temp = hourly.value("temperature_2m").toList();
    const QVariantMap current = reply.value("current").toMap();
    const uint now = current.value("time").toUInt();
    const uint t0 = localDayStart(now, reply.value("utc_offset_seconds").toInt());
    const PvCurve::Plane testPlane = plane(2.0, 0.85);

    QVERIFY(PvCurve::temperatureComplete(times, temp, t0));
    QVariantMap curve;
    QVERIFY(PvCurve::build(times, hourly.value("global_tilted_irradiance").toList(), temp, t0, testPlane, curve));
    QCOMPARE(curve.value("corrections").toList(), QVariantList{QStringLiteral("temperature")});
    const QVariantList p = curve.value("p").toList();

    // Sunset of 2026-10-15 from the site reply recorded in the same minute.
    const uint sunset = 1792082353;
    const QVariantMap s = PvCurve::scalars(reply, t0, sunset, testPlane, true);

    auto sumSlots = [&](uint from, uint to) {   // slots whose interval lies in [from, to)
        double wh = 0;
        for (int i = 0; i < 48; i++) {
            const uint start = t0 + i * 3600;
            if (start >= from && start + 3600 <= to)
                wh += p.at(i).toDouble();
        }
        return qRound(wh / 100.0) / 10.0;
    };
    // The first whole future hour starts at the next full hour after now.
    const uint next = (now / 3600 + 1) * 3600;
    QCOMPARE(next - now, 1800u);
    // ...but the bucket holding now (07:00-08:00) is the first one strictly after now in
    // the API's convention (sample 08:00 = mean of 07:00-08:00): the windows start there.
    const uint first = next - 3600;
    QCOMPARE(s.value("energyNext3h").toDouble(), sumSlots(first, first + 3 * 3600));
    QCOMPARE(s.value("energyNext6h").toDouble(), sumSlots(first, first + 6 * 3600));
    QCOMPARE(s.value("energyNext12h").toDouble(), sumSlots(first, first + 12 * 3600));
    QCOMPARE(s.value("energyTomorrow").toDouble(), sumSlots(t0 + 86400, t0 + 2 * 86400));
    // Sunset 18:39 local: the 18:00-19:00 bucket is included, 19:00 on is not.
    QCOMPARE(s.value("energyRestOfDay").toDouble(), sumSlots(first, t0 + 19 * 3600));
    QVERIFY(s.value("energyTomorrow").toDouble() > 0);

    // pvPower: current GTI 0 at 07:30, so 0 either way - the instant itself is checked by
    // hand on a synthetic reply below.
    QCOMPARE(s.value("pvPower").toDouble(), 0.0);

    QVariantMap synthetic = reply;
    QVariantMap cur = current;
    cur.insert("global_tilted_irradiance", 800.0);
    cur.insert("temperature_2m", -5.0);
    synthetic.insert("current", cur);
    // 800 W/m2 at -5 C: factor 1.0185 (referenceValues) ; 800 * 2 * 0.85 * 1.0185 =
    // 1385.16 W -> 1400 W at the 50 W quantum. Uncorrected: 1360 -> 1350.
    QCOMPARE(PvCurve::scalars(synthetic, t0, sunset, testPlane, true).value("pvPower").toDouble(), 1400.0);
    QCOMPARE(PvCurve::scalars(synthetic, t0, sunset, testPlane, false).value("pvPower").toDouble(), 1350.0);
}

void TestPvCurve::scalarsFollowTheDecisionOfTheRun()
{
    // One temperature hole in the window: the curve goes uncorrected, and so must every
    // scalar - their energies are then the sums of the v1 p, bit for bit.
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-15-elevation153.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    QVariantList temp = hourly.value("temperature_2m").toList();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());
    temp[times.indexOf(QVariant(t0 + 38 * 3600))] = QVariant();
    hourly.insert("temperature_2m", temp);
    QVariantMap holed = reply;
    holed.insert("hourly", hourly);

    QVERIFY(!PvCurve::temperatureComplete(times, temp, t0));
    const PvCurve::Plane testPlane = plane(2.0, 0.85);
    const QVariantMap s = PvCurve::scalars(holed, t0, 0, testPlane, false);
    const QVariantList v1 = v1Points(times, gti, t0, 2.0, 0.85);
    double tomorrow = 0;
    for (int i = 24; i < 48; i++)
        tomorrow += v1.at(i).toDouble();
    QCOMPARE(s.value("energyTomorrow").toDouble(), qRound(tomorrow / 100.0) / 10.0);
    // And the corrected figure differs on this day, so the test can tell the two apart.
    const QVariantMap c = PvCurve::scalars(reply, t0, 0, testPlane, true);
    QVERIFY(c.value("energyTomorrow") != s.value("energyTomorrow"));
    // sunset 0 = unknown: no rest-of-day rather than one bounded by nothing.
    QVERIFY(!s.contains("energyRestOfDay"));
}

void TestPvCurve::pvPowerIsLeftOutWithoutCurrentTemperature()
{
    // The 2026-10-13 reply was recorded before etm16 asked current=temperature_2m: a real
    // corrected run with no current temperature. pvPower is left out (not updated) rather
    // than published uncorrected beside corrected energies; the energies are still there.
    const QVariantMap reply = loadFixture("plane-meteofrance_seamless-2026-10-13.json");
    QVERIFY(!reply.value("current").toMap().contains("temperature_2m"));
    const QVariantMap hourly = reply.value("hourly").toMap();
    const uint t0 = localDayStart(reply.value("current").toMap().value("time").toUInt(),
                                  reply.value("utc_offset_seconds").toInt());
    QVERIFY(PvCurve::temperatureComplete(hourly.value("time").toList(), hourly.value("temperature_2m").toList(), t0));
    const QVariantMap s = PvCurve::scalars(reply, t0, 0, plane(), true);
    QVERIFY(!s.contains("pvPower"));
    QVERIFY(s.contains("gtiSource"));
    QVERIFY(s.contains("energyTomorrow"));
    QVERIFY(PvCurve::scalars(reply, t0, 0, plane(), false).contains("pvPower"));
}

void TestPvCurve::elevationEffectOnARealReply()
{
    // Two replies recorded in the same minute (2026-10-15 07:44 local,
    // tilt 50 South, meteofrance_seamless), the plane request of etm15 and of etm16: the
    // second sends elevation=153, the declared value, against 156 m for the model's
    // terrain. This is the one change etm16 makes to what the plane collects: measured here.
    const QVariantMap before = loadFixture("plane-meteofrance_seamless-2026-10-15-noelevation.json");
    const QVariantMap after = loadFixture("plane-meteofrance_seamless-2026-10-15-elevation153.json");
    QCOMPARE(before.value("elevation").toDouble(), 156.0);
    QCOMPARE(after.value("elevation").toDouble(), 153.0);
    const QVariantMap hb = before.value("hourly").toMap();
    const QVariantMap ha = after.value("hourly").toMap();
    QCOMPARE(ha.value("time"), hb.value("time"));
    // GTI does not depend on the elevation: every sample identical.
    QCOMPARE(ha.value("global_tilted_irradiance"), hb.value("global_tilted_irradiance"));

    const uint t0 = localDayStart(after.value("current").toMap().value("time").toUInt(),
                                  after.value("utc_offset_seconds").toInt());
    const PvCurve::Plane testPlane = plane(2.0, 0.85);
    QVariantMap cb, ca;
    QVERIFY(PvCurve::build(hb.value("time").toList(), hb.value("global_tilted_irradiance").toList(),
                           hb.value("temperature_2m").toList(), t0, testPlane, cb));
    QVERIFY(PvCurve::build(ha.value("time").toList(), ha.value("global_tilted_irradiance").toList(),
                           ha.value("temperature_2m").toList(), t0, testPlane, ca));
    QCOMPARE(ca.value("gti"), cb.value("gti"));
    double maxDt = 0, sumDp = 0, maxDp = 0, eb = 0;
    int changedT = 0, changedP = 0;
    for (int i = 0; i < 48; i++) {
        const double dt = ca.value("tempC").toList().at(i).toDouble() - cb.value("tempC").toList().at(i).toDouble();
        const double dp = ca.value("p").toList().at(i).toDouble() - cb.value("p").toList().at(i).toDouble();
        maxDt = qMax(maxDt, qAbs(dt));
        maxDp = qMax(maxDp, qAbs(dp));
        sumDp += dp;
        eb += cb.value("p").toList().at(i).toDouble();
        changedT += qFuzzyIsNull(dt) ? 0 : 1;
        changedP += qFuzzyIsNull(dp) ? 0 : 1;
    }
    // 3 m lower: at most a tenth of a kelvin warmer, never colder.
    QVERIFY(maxDt <= 0.1 + 1e-9);
    qInfo("elevation 153 vs 156 m, test plane: tempC changed on %d/48 slots (max %.1f K), "
          "p changed on %d/48 slots (max %.0f W), 48 h energy %+.0f Wh of %.0f Wh (%+.3f %%)",
          changedT, maxDt, changedP, maxDp, sumDp, eb, 100.0 * sumDp / eb);
}

QTEST_APPLESS_MAIN(TestPvCurve)
#include "tst_pvcurve.moc"
