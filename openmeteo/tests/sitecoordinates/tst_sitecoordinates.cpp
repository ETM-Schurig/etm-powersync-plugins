// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline tests of the site coordinates rule (CONTRACT_A sec. 4): a site without entered
// coordinates is refused, never forecast for a placeholder point. No nymea, no network.

#include "sitecoordinates.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <limits>

namespace {

const double Untouched = -12345.0;

// Runs the rule and checks that a refusal leaves the outputs as they were.
bool accepts(const QVariant &lat, const QVariant &lon, double *outLat = nullptr, double *outLon = nullptr)
{
    double la = Untouched, lo = Untouched;
    const bool ok = SiteCoordinates::read(lat, lon, la, lo);
    if (!ok && (la != Untouched || lo != Untouched))
        qFatal("a refused pair wrote its outputs");
    if (outLat)
        *outLat = la;
    if (outLon)
        *outLon = lo;
    return ok;
}

// The null a missing key leaves once nymea converts it to the param type
// (thingmanagerimplementation.cpp, loadConfiguredThings).
QVariant convertedMissingKey()
{
    QVariant v;
    v.convert(QMetaType(QMetaType::Double));
    return v;
}

QJsonObject siteParam(const QString &name)
{
    QFile file(QStringLiteral(PLUGIN_JSON));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("cannot open the plugin JSON");
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    foreach (const QJsonValue &vendor, root.value("vendors").toArray()) {
        foreach (const QJsonValue &thingClass, vendor.toObject().value("thingClasses").toArray()) {
            if (thingClass.toObject().value("name").toString() != QLatin1String("openMeteoSite"))
                continue;
            foreach (const QJsonValue &param, thingClass.toObject().value("paramTypes").toArray()) {
                if (param.toObject().value("name").toString() == name)
                    return param.toObject();
            }
        }
    }
    return QJsonObject();
}

}

class TestSiteCoordinates : public QObject
{
    Q_OBJECT

private slots:
    void validCoordinatesAreAccepted();
    void absentCoordinatesAreRefused();
    void theSentinelIsRefused();
    void outOfRangeIsRefused();
    void exactBoundsAreAccepted();
    void zeroZeroIsARealPoint();
    void theDeclaredDefaultIsTheSentinel();
};

void TestSiteCoordinates::validCoordinatesAreAccepted()
{
    double lat, lon;
    QVERIFY(accepts(48.85, 2.35, &lat, &lon));
    QCOMPARE(lat, 48.85);
    QCOMPARE(lon, 2.35);
    QVERIFY(accepts(-33.87, 151.21, &lat, &lon));
    QCOMPARE(lat, -33.87);
    QCOMPARE(lon, 151.21);
    // As stored in things.conf and read back: a string that holds a number.
    QVERIFY(accepts(QStringLiteral("45.5"), QStringLiteral("-73.6"), &lat, &lon));
    QCOMPARE(lat, 45.5);
    QCOMPARE(lon, -73.6);
}

void TestSiteCoordinates::absentCoordinatesAreRefused()
{
    QVERIFY(!accepts(QVariant(), 2.35));
    QVERIFY(!accepts(48.85, QVariant()));
    QVERIFY(!accepts(QVariant(), QVariant()));
    QVERIFY(!accepts(convertedMissingKey(), 2.35));
    QVERIFY(!accepts(48.85, convertedMissingKey()));
    // Present but not a number.
    QVERIFY(!accepts(QStringLiteral(""), 2.35));
    QVERIFY(!accepts(48.85, QStringLiteral("east")));
}

void TestSiteCoordinates::theSentinelIsRefused()
{
    const double unset = SiteCoordinates::Unset;
    QVERIFY(!accepts(unset, unset));
    // One coordinate entered is not enough.
    QVERIFY(!accepts(48.85, unset));
    QVERIFY(!accepts(unset, 2.35));
}

void TestSiteCoordinates::outOfRangeIsRefused()
{
    QVERIFY(!accepts(90.01, 0.0));
    QVERIFY(!accepts(-90.01, 0.0));
    QVERIFY(!accepts(0.0, 180.01));
    QVERIFY(!accepts(0.0, -180.01));
    // Swapped by the installer: a longitude in the latitude field.
    QVERIFY(!accepts(151.21, -33.87));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    QVERIFY(!accepts(nan, 0.0));
    QVERIFY(!accepts(0.0, nan));
    QVERIFY(!accepts(inf, 0.0));
    QVERIFY(!accepts(0.0, -inf));
}

void TestSiteCoordinates::exactBoundsAreAccepted()
{
    double lat, lon;
    QVERIFY(accepts(90.0, 180.0, &lat, &lon));
    QCOMPARE(lat, 90.0);
    QCOMPARE(lon, 180.0);
    QVERIFY(accepts(-90.0, -180.0, &lat, &lon));
    QCOMPARE(lat, -90.0);
    QCOMPARE(lon, -180.0);
    QVERIFY(accepts(90.0, -180.0));
    QVERIFY(accepts(-90.0, 180.0));
}

void TestSiteCoordinates::zeroZeroIsARealPoint()
{
    double lat = Untouched, lon = Untouched;
    QVERIFY(accepts(0.0, 0.0, &lat, &lon));
    QCOMPARE(lat, 0.0);
    QCOMPARE(lon, 0.0);
    QVERIFY(accepts(0, 0));
}

void TestSiteCoordinates::theDeclaredDefaultIsTheSentinel()
{
    // A site added without touching the fields gets the declared defaults
    // (buildParams). They must be the sentinel, and refused.
    const QJsonObject latitude = siteParam(QStringLiteral("latitude"));
    const QJsonObject longitude = siteParam(QStringLiteral("longitude"));
    QVERIFY(!latitude.isEmpty());
    QVERIFY(!longitude.isEmpty());
    QCOMPARE(latitude.value("defaultValue").toDouble(), SiteCoordinates::Unset);
    QCOMPARE(longitude.value("defaultValue").toDouble(), SiteCoordinates::Unset);
    QVERIFY(!accepts(latitude.value("defaultValue").toVariant(), longitude.value("defaultValue").toVariant()));
    // No declared bounds: nymea would refuse the sentinel itself, before setupThing, with
    // no message for the installer (thingutils.cpp, verifyParam).
    QVERIFY(!latitude.contains("minValue") && !latitude.contains("maxValue"));
    QVERIFY(!longitude.contains("minValue") && !longitude.contains("maxValue"));
}

QTEST_APPLESS_MAIN(TestSiteCoordinates)
#include "tst_sitecoordinates.moc"
