// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of the ensemble spread (CONTRACT_A sec. 1.5 and 2.5). The fixture is the
// reply of ensemble-api.open-meteo.com recorded 2026-10-15 07:44 local, icon_seamless,
// tilt 50 South, coordinates replaced (../fixtures/README.md). No test opens a socket.

#include "ensemblespread.h"

#include <QFile>
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

// Population CV of the daily sums, by instant, for the local day starting at dayStart.
double referenceCv(const QVariantMap &reply, uint dayStart)
{
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    QList<double> totals;
    for (auto it = hourly.constBegin(); it != hourly.constEnd(); ++it) {
        if (!it.key().contains("member"))
            continue;
        double sum = 0;
        for (int i = 0; i < times.count(); i++) {
            const uint t = times.at(i).toUInt();
            if (t > dayStart && t <= dayStart + 86400)
                sum += it.value().toList().at(i).toDouble();
        }
        totals.append(sum);
    }
    double mean = 0;
    for (double s : totals)
        mean += s / totals.count();
    double var = 0;
    for (double s : totals)
        var += (s - mean) * (s - mean) / totals.count();
    return 100.0 * qSqrt(var) / mean;
}

}

class TestEnsembleSpread : public QObject
{
    Q_OBJECT

private slots:
    void spreadOnARealReply();
    void aDayThatCannotBeComputedIsNull();
    void emptyReplyIsAFailure();
    void freshnessIsTwiceTheInterval();
    void invalidIsNullInJsonNeverZero();
};

void TestEnsembleSpread::spreadOnARealReply()
{
    const QVariantMap reply = loadFixture("ensemble-icon_seamless-2026-10-15.json");
    const QVariantList spread = EnsembleSpread::daily(reply);
    QCOMPARE(spread.count(), 2);
    const uint today = 1792015200u;   // 2026-10-15 00:00 local
    for (int day = 0; day < 2; day++) {
        QVERIFY(!spread.at(day).isNull());
        const double expected = qRound(referenceCv(reply, today + day * 86400) * 10) / 10.0;
        QCOMPARE(spread.at(day).toDouble(), expected);
        qInfo("day %d: spread %.1f %%", day, spread.at(day).toDouble());
    }
}

void TestEnsembleSpread::aDayThatCannotBeComputedIsNull()
{
    // Tomorrow cut off on every member: today is still published, tomorrow is null - and
    // never the previous value, which forecastWeatherSpreadTime would date as fresh.
    QVariantMap reply = loadFixture("ensemble-icon_seamless-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    for (auto it = hourly.begin(); it != hourly.end(); ++it) {
        if (!it.key().contains("member"))
            continue;
        QVariantList values = it.value().toList();
        for (int i = 0; i < times.count(); i++) {
            if (times.at(i).toUInt() == 1792015200u + 86400 + 12 * 3600)
                values[i] = QVariant();
        }
        it.value() = values;
    }
    reply.insert("hourly", hourly);
    const QVariantList spread = EnsembleSpread::daily(reply);
    QVERIFY(!spread.at(0).isNull());
    QVERIFY(spread.at(1).isNull());
}

void TestEnsembleSpread::emptyReplyIsAFailure()
{
    QVERIFY(EnsembleSpread::daily(QVariantMap()).isEmpty());
    QVariantMap reply = loadFixture("ensemble-icon_seamless-2026-10-15.json");
    QVariantMap hourly = reply.value("hourly").toMap();
    for (const QString &key : hourly.keys()) {
        if (key.contains("member"))
            hourly.remove(key);
    }
    reply.insert("hourly", hourly);
    QVERIFY(EnsembleSpread::daily(reply).isEmpty());
}

void TestEnsembleSpread::freshnessIsTwiceTheInterval()
{
    QCOMPARE(EnsembleSpread::MaxAgeS, 21600);
    const qint64 t = 1792042200;
    QVERIFY(!EnsembleSpread::fresh(0, t));            // never computed
    QVERIFY(EnsembleSpread::fresh(t, t));
    QVERIFY(EnsembleSpread::fresh(t, t + 21600));      // exactly 2 x 3 h: still current
    QVERIFY(!EnsembleSpread::fresh(t, t + 21601));     // beyond: invalid
}

void TestEnsembleSpread::invalidIsNullInJsonNeverZero()
{
    // What nymea does with the value (libnymea Thing::setStateValue): it converts the
    // QVariant to the state type and discards the change when that fails. An invalid
    // QVariant fails, so null cannot be published; NaN passes.
    QVariant null;
    QVERIFY(!null.convert(QMetaType(QMetaType::Double)));
    QVariant nan(EnsembleSpread::invalid());
    QVERIFY(nan.convert(QMetaType(QMetaType::Double)));
    QVERIFY(qIsNaN(nan.toDouble()));
    // And what the client receives: nymea serialises with QJsonDocument::fromVariant(..)
    // .toJson() (jsonrpcserverimplementation.cpp), which writes NaN as null.
    const QByteArray json = QJsonDocument::fromVariant(QVariantMap{{"value", nan}}).toJson(QJsonDocument::Compact);
    QCOMPARE(json, QByteArray("{\"value\":null}"));
    // NaN never equals itself: the plugin guards before writing it again (one
    // notification, not one per tick).
    QVERIFY(QVariant(EnsembleSpread::invalid()) != QVariant(EnsembleSpread::invalid()));
}

QTEST_APPLESS_MAIN(TestEnsembleSpread)
#include "tst_ensemblespread.moc"
