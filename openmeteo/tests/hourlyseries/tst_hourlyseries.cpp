// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of the hourly series states (CONTRACT_A sec. 1.7 and following).
//
// Fixtures recorded 2026-10-15 07:44 local with the requests the plugin sends, coordinates
// replaced by an example point (../fixtures/README.md). No test opens a socket. Every reference
// is looked up by instant here, independently of the code under test.

#include "hourlyseries.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QTest>

namespace {

QVariantMap loadFixture(const QString &name)
{
    QFile file(QStringLiteral(FIXTURES_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("fixture %s not readable", qPrintable(name));
    return QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
}

QHash<uint, QVariant> byInstant(const QVariantList &times, const QVariantList &values)
{
    QHash<uint, QVariant> map;
    for (int i = 0; i < times.count(); i++)
        map.insert(times.at(i).toUInt(), values.at(i));
    return map;
}

// A satellite-like reply on one fixed offset, as the API answers (one utc_offset for the
// whole range, even across a DST change), each sample encoding its own instant.
QVariantMap syntheticSatellite(uint first, int count, int utcOffset)
{
    QVariantList times, gti;
    for (int i = 0; i < count; i++) {
        const uint at = first + i * 3600;
        times.append(at);
        gti.append(double((at / 3600) % 1000));
    }
    QVariantMap hourly;
    hourly.insert("time", times);
    hourly.insert("global_tilted_irradiance", gti);
    QVariantMap reply;
    reply.insert("utc_offset_seconds", utcOffset);
    reply.insert("hourly", hourly);
    return reply;
}

}

class TestHourlySeries : public QObject
{
    Q_OBJECT

private slots:
    void observedSeriesOnARealReply();
    void observedSeriesAgreesWithObservedGtiTime();
    void observedSeriesReadsByInstant();
    void observedSeriesNullIsNotZero();
    void observedSeriesOverDstDays();
    void observedSeriesEmptyReplyIsAFailure();
    void siteWeatherOnARealReply();
    void siteWeatherAgreesWithCurrent();
    void siteWeatherReadsByInstant();
    void siteWeatherClearSkyZeroIsNotAbsence();
    void siteWeatherWithoutWeatherCodeIsNotZeros();
    void siteWeatherOverDstDays();
    void temperatureIsInstantaneousOnARealReply();
};

void TestHourlySeries::observedSeriesOnARealReply()
{
    const QVariantMap reply = loadFixture("satellite-plane-2026-10-15.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QHash<uint, QVariant> g = byInstant(times, hourly.value("global_tilted_irradiance").toList());

    const QVariantMap s = HourlySeries::observedGti(reply);
    QCOMPARE(s.value("v").toInt(), 1);
    QCOMPARE(s.value("step").toInt(), 3600);
    QCOMPARE(s.value("unit").toString(), QStringLiteral("W/m2"));
    QCOMPARE(s.value("convention").toString(), QStringLiteral("mean over [t, t+step)"));
    QCOMPARE(s.value("regime").toString(), QStringLiteral("observed"));
    QCOMPARE(s.value("source").toString(), QStringLiteral("satellite_radiation_seamless"));
    // Yesterday 00:00 local (Europe/Paris, +02:00) = 2026-10-13 22:00Z.
    QCOMPARE(s.value("t0").toUInt(), 1791928800u);
    const QVariantList gti = s.value("gti").toList();
    QCOMPARE(gti.count(), 48);
    for (int i = 0; i < 48; i++) {
        const uint end = 1791928800u + (i + 1) * 3600;
        QCOMPARE(gti.at(i).isNull(), !g.contains(end) || g.value(end).isNull());
        if (!gti.at(i).isNull())
            QCOMPARE(gti.at(i).toDouble(), g.value(end).toDouble());
    }
    // Yesterday's brightest hour, 824.0 W/m2 at the sample 2026-10-14 14:00 local, is
    // the mean of 13:00-14:00 local: slot 13 of yesterday.
    QCOMPARE(gti.at(13).toDouble(), 824.0);
    // Observed up to the sample of 07:00 local today (the mean of 06:00-07:00), slot 30;
    // nothing after: the satellite lags about an hour.
    QCOMPARE(gti.at(30).toDouble(), 0.0);
    for (int i = 31; i < 48; i++)
        QVERIFY(gti.at(i).isNull());
}

void TestHourlySeries::observedSeriesAgreesWithObservedGtiTime()
{
    // ems etm69 attaches observedGti to the slot targetAt = observedGtiTime - 3600
    // (its documented choice). The series must put the same value in the same slot.
    const QVariantMap reply = loadFixture("satellite-plane-2026-10-15.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList values = hourly.value("global_tilted_irradiance").toList();
    int last = -1;   // what updatePlaneSatellite publishes as observedGti / observedGtiTime
    for (int i = times.count() - 1; i >= 0 && last < 0; i--) {
        if (!values.at(i).isNull())
            last = i;
    }
    QVERIFY(last >= 0);
    const uint observedGtiTime = times.at(last).toUInt();
    const QVariantMap s = HourlySeries::observedGti(reply);
    const int slot = int((observedGtiTime - 3600 - s.value("t0").toUInt()) / 3600);
    QCOMPARE(s.value("gti").toList().at(slot).toDouble(), values.at(last).toDouble());
    QVERIFY(s.value("gti").toList().at(slot + 1).isNull());
}

void TestHourlySeries::observedSeriesReadsByInstant()
{
    // One hour removed from the middle of the reply: an index walk would shift every
    // later slot by one hour; the lookup by instant leaves exactly one hole.
    QVariantMap reply = loadFixture("satellite-plane-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    QVariantList times = hourly.value("time").toList();
    QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const uint t0 = 1791928800u;
    const int removed = times.indexOf(QVariant(t0 + 15 * 3600));
    QVERIFY(removed > 0);
    const QHash<uint, QVariant> g = byInstant(times, gti);
    times.removeAt(removed);
    gti.removeAt(removed);
    hourly.insert("time", times);
    hourly.insert("global_tilted_irradiance", gti);
    reply.insert("hourly", hourly);

    const QVariantList s = HourlySeries::observedGti(reply).value("gti").toList();
    QVERIFY(s.at(14).isNull());
    for (int i = 0; i < 31; i++) {
        if (i != 14)
            QCOMPARE(s.at(i).toDouble(), g.value(t0 + (i + 1) * 3600).toDouble());
    }
}

void TestHourlySeries::observedSeriesNullIsNotZero()
{
    // Night is a real 0, published 0; a missing observation is null, never 0.
    QVariantMap reply = loadFixture("satellite-plane-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    QVariantList gti = hourly.value("global_tilted_irradiance").toList();
    const QVariantList times = hourly.value("time").toList();
    const int night = times.indexOf(QVariant(1791928800u + 3 * 3600));   // 03:00 local, 0.0
    QCOMPARE(gti.at(night).toDouble(), 0.0);
    gti[night] = QVariant();
    hourly.insert("global_tilted_irradiance", gti);
    reply.insert("hourly", hourly);
    const QVariantList s = HourlySeries::observedGti(reply).value("gti").toList();
    QVERIFY(s.at(2).isNull());
    QVERIFY(!s.at(1).isNull());
    QCOMPARE(s.at(1).toDouble(), 0.0);
    // And through JSON, as nymea sends it: null, not 0.
    const QByteArray json = QJsonDocument::fromVariant(QVariantMap{{"gti", s}}).toJson(QJsonDocument::Compact);
    QVERIFY(json.startsWith("{\"gti\":[0,0,null,0"));
}

void TestHourlySeries::observedSeriesOverDstDays()
{
    // 2026-10-25 (25 h local day) and 2026-03-29 (23 h): the API answers on one fixed
    // offset for the whole range, so the window is 48 absolute hours from the reply's
    // first midnight, whatever the length of the local day. SYNTHETIC: the satellite
    // archive does not serve those dates when recorded.
    struct { uint first; int offset; } cases[] = {
        { 1792792800u, 7200 },   // 2026-10-24 00:00 CEST: yesterday of the 25 h day
        { 1774652400u, 3600 },   // 2026-03-28 00:00 CET: yesterday of the 23 h day
    };
    for (const auto &c : cases) {
        const QVariantMap s = HourlySeries::observedGti(syntheticSatellite(c.first, 48, c.offset));
        QCOMPARE(s.value("t0").toUInt(), c.first);
        const QVariantList gti = s.value("gti").toList();
        for (int i = 0; i < 47; i++)
            QCOMPARE(gti.at(i).toDouble(), double(((c.first + (i + 1) * 3600) / 3600) % 1000));
        QVERIFY(gti.at(47).isNull());   // the sample at the end of the window is not in the reply
    }
}

void TestHourlySeries::observedSeriesEmptyReplyIsAFailure()
{
    // A 200 that carries nothing must read as a failure (empty), never as 48 zeros.
    QVERIFY(HourlySeries::observedGti(QVariantMap()).isEmpty());
    QVariantMap reply = loadFixture("satellite-plane-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    QVariantList nulls;
    for (int i = 0; i < hourly.value("time").toList().count(); i++)
        nulls.append(QVariant());
    hourly.insert("global_tilted_irradiance", nulls);
    reply.insert("hourly", hourly);
    QVERIFY(HourlySeries::observedGti(reply).isEmpty());
    hourly.remove("global_tilted_irradiance");
    reply.insert("hourly", hourly);
    QVERIFY(HourlySeries::observedGti(reply).isEmpty());
}

void TestHourlySeries::siteWeatherOnARealReply()
{
    const QVariantMap reply = loadFixture("site-meteofrance_seamless-2026-10-15.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QHash<uint, QVariant> wc = byInstant(times, hourly.value("weather_code").toList());
    const QHash<uint, QVariant> day = byInstant(times, hourly.value("is_day").toList());
    const QHash<uint, QVariant> temp = byInstant(times, hourly.value("temperature_2m").toList());

    const QVariantMap s = HourlySeries::siteWeather(reply);
    QCOMPARE(s.value("v").toInt(), 1);
    QCOMPARE(s.value("step").toInt(), 3600);
    QCOMPARE(s.value("regime").toString(), QStringLiteral("forecast"));
    // Today 00:00 local, an absolute instant (2026-10-14 22:00Z), on an hour boundary.
    QCOMPARE(s.value("t0").toUInt(), 1792015200u);
    QCOMPARE(s.value("t0").toUInt() % 3600, 0u);
    const QVariantMap convention = s.value("convention").toMap();
    QCOMPARE(convention.value("weatherCode").toString(), QStringLiteral("WMO code, instantaneous at t"));
    QCOMPARE(convention.value("isDay").toString(), QStringLiteral("1 day, 0 night, instantaneous at t"));
    QCOMPARE(convention.value("tempC").toString(), QStringLiteral("temperature_2m in C, instantaneous at t"));

    const QVariantList w = s.value("weatherCode").toList();
    const QVariantList d = s.value("isDay").toList();
    const QVariantList t = s.value("tempC").toList();
    QCOMPARE(w.count(), 48);
    QCOMPARE(d.count(), 48);
    QCOMPARE(t.count(), 48);
    // The whole horizon of forecast_days=2 is covered, with nothing missing.
    for (int i = 0; i < 48; i++) {
        const uint at = 1792015200u + i * 3600;
        QCOMPARE(w.at(i).toInt(), wc.value(at).toInt());
        QCOMPARE(w.at(i).userType(), int(QMetaType::Int));
        QCOMPARE(d.at(i).toInt(), day.value(at).toInt());
        QCOMPARE(t.at(i).toDouble(), temp.value(at).toDouble());
    }
    // is_day is instantaneous at t: sunrise 07:48:50 local, so 07:00 is night and 08:00
    // is day; sunset 18:39:13, so 18:00 is day and 19:00 night. The slot 18:00-19:00
    // carries the 18:00 value, day, although sunset falls inside it.
    QCOMPARE(d.at(7).toInt(), 0);
    QCOMPARE(d.at(8).toInt(), 1);
    QCOMPARE(d.at(18).toInt(), 1);
    QCOMPARE(d.at(19).toInt(), 0);
}

void TestHourlySeries::siteWeatherAgreesWithCurrent()
{
    // The check made with bench_probe.py after installation, played on the recorded reply: the
    // code of the current hour agrees with weather_code of "current" (which feeds
    // weatherCondition), and so does the temperature at the slot start of 07:00.
    const QVariantMap reply = loadFixture("site-meteofrance_seamless-2026-10-15.json");
    const QVariantMap current = reply.value("current").toMap();
    const QVariantMap s = HourlySeries::siteWeather(reply);
    const int slot = int((current.value("time").toUInt() - s.value("t0").toUInt()) / 3600);
    QCOMPARE(slot, 7);   // 07:30 local
    QCOMPARE(s.value("weatherCode").toList().at(slot).toInt(), current.value("weather_code").toInt());
    QCOMPARE(s.value("isDay").toList().at(slot).toInt(), current.value("is_day").toInt());
}

void TestHourlySeries::siteWeatherReadsByInstant()
{
    // A reply that starts one hour late: an index walk would shift every value by one
    // hour; by instant, slot 0 is missing (null) and every other slot is unchanged.
    QVariantMap reply = loadFixture("site-meteofrance_seamless-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantMap full = HourlySeries::siteWeather(reply);
    for (const QString &key : hourly.keys()) {
        QVariantList values = hourly.value(key).toList();
        values.removeFirst();
        hourly.insert(key, values);
    }
    reply.insert("hourly", hourly);
    const QVariantMap s = HourlySeries::siteWeather(reply);
    QCOMPARE(s.value("t0"), full.value("t0"));
    for (const char *key : {"weatherCode", "isDay", "tempC"}) {
        QVERIFY(s.value(key).toList().at(0).isNull());
        QCOMPARE(s.value(key).toList().mid(1), full.value(key).toList().mid(1));
    }
}

void TestHourlySeries::siteWeatherClearSkyZeroIsNotAbsence()
{
    // The real reply has code 0 (clear sky) at 22:00 and 23:00 today. They stay 0. An
    // hour removed from the reply comes out null - in the map and in the JSON nymea
    // sends - never 0, which the app would draw as a sun (or a night, for isDay).
    QVariantMap reply = loadFixture("site-meteofrance_seamless-2026-10-15.json");
    const QVariantMap s = HourlySeries::siteWeather(reply);
    QCOMPARE(s.value("weatherCode").toList().at(22).toInt(), 0);
    QVERIFY(!s.value("weatherCode").toList().at(22).isNull());

    QVariantMap hourly = reply.value("hourly").toMap();
    for (const char *key : {"weather_code", "is_day", "temperature_2m"}) {
        QVariantList values = hourly.value(key).toList();
        values[23] = QVariant();
        hourly.insert(key, values);
    }
    reply.insert("hourly", hourly);
    const QVariantMap h = HourlySeries::siteWeather(reply);
    for (const char *key : {"weatherCode", "isDay", "tempC"}) {
        QVERIFY(h.value(key).toList().at(23).isNull());
        QVERIFY(!h.value(key).toList().at(22).isNull());
    }
    const QByteArray json = QJsonDocument::fromVariant(h.value("weatherCode")).toJson(QJsonDocument::Compact);
    QVERIFY(json.contains(",0,null,"));
}

void TestHourlySeries::siteWeatherWithoutWeatherCodeIsNotZeros()
{
    // A reply without hourly.weather_code: 48 nulls for the code, never 48 zeros (48
    // suns); the temperature, present, is still published.
    QVariantMap reply = loadFixture("site-meteofrance_seamless-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    hourly.remove("weather_code");
    reply.insert("hourly", hourly);
    const QVariantMap s = HourlySeries::siteWeather(reply);
    QCOMPARE(s.value("weatherCode").toList().count(), 48);
    for (const QVariant &v : s.value("weatherCode").toList())
        QVERIFY(v.isNull());
    QVERIFY(!s.value("tempC").toList().at(0).isNull());

    // A code series of another length cannot be placed on the instants: absent too.
    QVariantList shorter = loadFixture("site-meteofrance_seamless-2026-10-15.json")
            .value("hourly").toMap().value("weather_code").toList().mid(1);
    hourly.insert("weather_code", shorter);
    reply.insert("hourly", hourly);
    for (const QVariant &v : HourlySeries::siteWeather(reply).value("weatherCode").toList())
        QVERIFY(v.isNull());

    // None of the three: nothing published, a failure for the caller.
    hourly.remove("weather_code");
    hourly.remove("is_day");
    hourly.remove("temperature_2m");
    reply.insert("hourly", hourly);
    QVERIFY(HourlySeries::siteWeather(reply).isEmpty());
}

void TestHourlySeries::siteWeatherOverDstDays()
{
    // 25 h and 23 h local days. The API answers on one fixed offset for its whole range,
    // so the 48 slots are 48 absolute hours from t0; the app converts each instant to
    // local time on display. SYNTHETIC (no such date served when recorded): each value
    // encodes its own instant.
    struct { uint first; int offset; } cases[] = {
        { 1792879200u, 7200 },   // 2026-10-25 00:00 CEST
        { 1774738800u, 3600 },   // 2026-03-29 00:00 CET
    };
    for (const auto &c : cases) {
        QVariantList times, code, day, temp;
        for (int i = 0; i < 48; i++) {
            const uint at = c.first + i * 3600;
            times.append(at);
            code.append(double((at / 3600) % 100));
            day.append(double((at / 3600) % 2));
            temp.append(double((at / 3600) % 1000) / 10.0);
        }
        QVariantMap hourly;
        hourly.insert("time", times);
        hourly.insert("weather_code", code);
        hourly.insert("is_day", day);
        hourly.insert("temperature_2m", temp);
        QVariantMap reply;
        reply.insert("utc_offset_seconds", c.offset);
        reply.insert("current", QVariantMap{{"time", c.first + 12 * 3600}});
        reply.insert("hourly", hourly);
        const QVariantMap s = HourlySeries::siteWeather(reply);
        QCOMPARE(s.value("t0").toUInt(), c.first);
        for (int i = 0; i < 48; i++) {
            const uint at = c.first + i * 3600;
            QCOMPARE(s.value("weatherCode").toList().at(i).toInt(), int((at / 3600) % 100));
            QCOMPARE(s.value("isDay").toList().at(i).toInt(), int((at / 3600) % 2));
            QCOMPARE(s.value("tempC").toList().at(i).toDouble(), double((at / 3600) % 1000) / 10.0);
        }
    }
}

void TestHourlySeries::temperatureIsInstantaneousOnARealReply()
{
    // Measured, not only read in the documentation. Same minute as the site fixture,
    // hourly AND minutely_15 temperature_2m: the hourly value at T is the 15-min value at
    // T, not the mean of the preceding hour (which is what the irradiance series are).
    const QVariantMap reply = loadFixture("site-conventions-minutely15-2026-10-15.json");
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantMap quarter = reply.value("minutely_15").toMap();
    const QHash<uint, QVariant> q = byInstant(quarter.value("time").toList(), quarter.value("temperature_2m").toList());
    const QVariantList times = hourly.value("time").toList();
    const QVariantList temps = hourly.value("temperature_2m").toList();
    int instant = 0, precedingMean = 0, compared = 0;
    for (int i = 0; i < times.count(); i++) {
        const uint t = times.at(i).toUInt();
        if (!q.contains(t) || !q.contains(t - 2700))
            continue;
        compared++;
        if (qAbs(q.value(t).toDouble() - temps.at(i).toDouble()) < 0.05)
            instant++;
        const double mean = (q.value(t - 2700).toDouble() + q.value(t - 1800).toDouble()
                             + q.value(t - 900).toDouble() + q.value(t).toDouble()) / 4;
        if (qAbs(qRound(mean * 10) / 10.0 - temps.at(i).toDouble()) < 0.05)
            precedingMean++;
    }
    qInfo("temperature_2m: %d hours compared, %d equal the instant at T, %d the preceding-hour mean",
          compared, instant, precedingMean);
    QVERIFY(compared >= 40);
    QVERIFY(instant >= compared - 2);
    QVERIFY(precedingMean <= compared / 4);
}

QTEST_APPLESS_MAIN(TestHourlySeries)
#include "tst_hourlyseries.moc"
