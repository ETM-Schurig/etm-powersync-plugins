// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract, its price slots and the EJP calendar: deterministic, in the Europe/Paris
// time zone, at instants chosen by the test. No meter.

#include "contract.h"
#include "ejpcalendar.h"
#include "ejpdeclaration.h"
#include "priceschedule.h"
#include "regulatedtariff.h"

#include <QJsonDocument>
#include <QtTest>

namespace {

const QTimeZone &paris()
{
    static const QTimeZone zone("Europe/Paris");
    return zone;
}

QDateTime at(const QString &local)
{
    return QDateTime(QDate::fromString(local.left(10), Qt::ISODate), QTime::fromString(local.mid(11, 5), QStringLiteral("HH:mm")), paris());
}

Contract make(Contract::Type type, const QString &ranges = QStringLiteral("22:00-06:00"), int kva = 6)
{
    Contract::Settings settings;
    settings.type = type;
    settings.subscribedKva = kva;
    settings.offPeakRanges = ranges;
    QString error;
    const std::optional<Contract> contract = Contract::create(settings, RegulatedTariff::builtIn(), &error);
    if (!contract)
        qWarning() << error;
    return contract.value();
}

QString iso(const QDateTime &dateTime)
{
    return dateTime.toString(Qt::ISODate);
}

// Every schedule: contiguous slots, the first contains now, the last ends at the horizon.
void checkShape(const PriceSchedule &schedule)
{
    QVERIFY(!schedule.slotList().isEmpty());
    QVERIFY(schedule.slotList().first().start <= schedule.now());
    QVERIFY(schedule.now() < schedule.slotList().first().end);
    QCOMPARE(schedule.slotList().last().end, schedule.horizonEnd());
    for (int i = 0; i + 1 < schedule.slotList().size(); i++) {
        QCOMPARE(schedule.slotList().at(i).end, schedule.slotList().at(i + 1).start);
        QVERIFY(schedule.slotList().at(i).start < schedule.slotList().at(i).end);
    }
}

}

class TestContract : public QObject
{
    Q_OBJECT

private slots:
    void regulatedTariffBuiltIn();
    void regulatedTariffToVerifyByDefault();
    void regulatedPrices();
    void manualPrices();
    void manualPriceMissingRefused();
    void tempoRefused();
    void rangesParsed_data();
    void rangesParsed();
    void rangesRefused_data();
    void rangesRefused();
    void nominal();

    void baseSchedule();
    void hcHpTwoRangesOneAcrossMidnight();
    void hcHpDaylightSavingStart();
    void hcHpDaylightSavingEnd();
    void hcHpBoundaryInTheMissingHour();
    void ticOverridesCurrentSlot();

    void ejpNormalDayFromTic();
    void ejpNoticeFromTic();
    void ejpPeakFromTic();
    void ejpPeakEndsAtOne();
    void ejpUndeclaredDayPresumedNormal();
    void ejpOutOfSeason();
    void ejpDeclaredThenConfirmedByNotice();
    void ejpDeclaredThenConfirmedByPeak();
    void ejpDeclaredThenContradicted();
    void ejpPeakSeenNotDeclared();
    void ejpDeclareTwice();
    void ejpCancelBeforeAndAfterSeven();
    void ejpDeclareBetweenMidnightAndOne();
    void ejpDeclareOutOfSeason();
    void ejpDeclareNotEjpContract();
    void ejpDeclarationAfterRestart();
    void ejpPeakOfMarch31EndsInApril();
    void ejpSeasonCounterAfterRestart();
    void ejpSeasonPruned();
    void ejpSeasonCounterWithEnteredDays();
    void ejpSeasonCounterInstalledAfterMidnight();
    void ejpSeasonCounterCountsFromSeven();
    void ejpSeasonEnteredDaysOfAnotherSeason();
    void ejpTicOutsideWindowsIgnored();

    void transportJson();
    void subscribedPowerFromIsousc_data();
    void subscribedPowerFromIsousc();
};

void TestContract::regulatedTariffBuiltIn()
{
    const RegulatedTariff &tariff = RegulatedTariff::builtIn();
    QVERIFY(tariff.isValid());
    QCOMPARE(tariff.effectiveDate(), QDate(2026, 8, 1));
    QVERIFY(tariff.toVerify());
    QVERIFY(tariff.source().contains(QStringLiteral("CRE")));
}

void TestContract::regulatedTariffToVerifyByDefault()
{
    const QByteArray json = R"({"schema":"etm.edf.regulatedTariff","version":1,"effectiveDate":"2027-02-01",
        "options":{"hchp":{"periods":{"hc":1,"hp":2}}}})";
    const RegulatedTariff tariff = RegulatedTariff::fromJson(json);
    QVERIFY(tariff.isValid());
    QVERIFY(tariff.toVerify()); // absent: still to verify
    QVERIFY(!RegulatedTariff::fromJson(R"({"schema":"other","version":1})").isValid());
    QVERIFY(!RegulatedTariff::fromJson("{broken").isValid());
}

void TestContract::regulatedPrices()
{
    QCOMPARE(make(Contract::Type::Base, QString(), 6).price("base"), 20.01);
    QCOMPARE(make(Contract::Type::Base, QString(), 3).price("base"), 20.01);
    QCOMPARE(make(Contract::Type::Base, QString(), 9).price("base"), 19.85);
    QCOMPARE(make(Contract::Type::Base, QString(), 36).price("base"), 19.85);
    const Contract hchp = make(Contract::Type::HcHp);
    QCOMPARE(hchp.price("hc"), 15.89);
    QCOMPARE(hchp.price("hp"), 21.42);
    const Contract ejp = make(Contract::Type::Ejp, QString(), 12);
    QCOMPARE(ejp.price("hn"), 17.89);
    QCOMPARE(ejp.price("pm"), 43.24);
    QCOMPARE(ejp.periods(), QStringList({ "hn", "pm" }));
}

void TestContract::manualPrices()
{
    Contract::Settings settings;
    settings.type = Contract::Type::HcHp;
    settings.priceSource = Contract::PriceSource::Manual;
    settings.offPeakRanges = QStringLiteral("23:00-07:00");
    settings.manualPrices = { { "hc", 12.5 }, { "hp", 18.0 }, { "base", 99 } };
    QString error;
    const std::optional<Contract> contract = Contract::create(settings, RegulatedTariff::builtIn(), &error);
    QVERIFY2(contract, qPrintable(error));
    QCOMPARE(contract->price("hc"), 12.5);
    QCOMPARE(contract->price("hp"), 18.0);
    QVERIFY(!contract->hasPeriod("base"));
    QCOMPARE(contract->priceSource(), Contract::PriceSource::Manual);
}

void TestContract::manualPriceMissingRefused()
{
    Contract::Settings settings;
    settings.type = Contract::Type::Ejp;
    settings.priceSource = Contract::PriceSource::Manual;
    settings.manualPrices = { { "hn", 17.0 } }; // pm not entered (0)
    QString error;
    QVERIFY(!Contract::create(settings, RegulatedTariff::builtIn(), &error));
    QVERIFY2(error.contains(QStringLiteral("pointe mobile")), qPrintable(error));
    settings.manualPrices.insert("pm", -3);
    QVERIFY(!Contract::create(settings, RegulatedTariff::builtIn(), &error));
}

void TestContract::tempoRefused()
{
    Contract::Settings settings;
    settings.type = Contract::Type::Tempo;
    QString error;
    QVERIFY(!Contract::create(settings, RegulatedTariff::builtIn(), &error));
    QVERIFY2(error.contains(QStringLiteral("Tempo")) && error.contains(QStringLiteral("pas encore")), qPrintable(error));
    // Even with every price entered by hand.
    settings.priceSource = Contract::PriceSource::Manual;
    settings.manualPrices = { { "hcjb", 1 }, { "hpjb", 2 } };
    QVERIFY(!Contract::create(settings, RegulatedTariff::builtIn(), &error));
}

void TestContract::rangesParsed_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("minutes");
    QTest::newRow("one, across midnight") << "22:00-06:00" << 1 << 480;
    QTest::newRow("two, comma") << "12:00-14:00, 22:00-04:00" << 2 << 480;
    QTest::newRow("two, semicolon, spaces") << " 01:30 - 07:30 ; 12:30-14:30 " << 2 << 480;
    QTest::newRow("to 24:00") << "16:00-24:00" << 1 << 480;
    QTest::newRow("to 00:00") << "16:00-00:00" << 1 << 480;
}

void TestContract::rangesParsed()
{
    QFETCH(QString, text);
    QFETCH(int, count);
    QFETCH(int, minutes);
    QString error;
    const std::optional<QList<Contract::Range>> ranges = Contract::parseRanges(text, &error);
    QVERIFY2(ranges, qPrintable(error));
    QCOMPARE(ranges->size(), count);
    int total = 0;
    for (const Contract::Range &range : *ranges)
        total += range.minutes();
    QCOMPARE(total, minutes);
}

void TestContract::rangesRefused_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    QTest::newRow("empty") << "" << "Saisissez";
    QTest::newRow("unreadable") << "22h-6h" << "illisible";
    QTest::newRow("invalid hour") << "25:00-06:00" << "invalide";
    QTest::newRow("same start and end") << "22:00-22:00" << "vide ou couvre 24 h";
    QTest::newRow("overlap") << "22:00-06:00, 05:00-07:00" << "chevauchent à 05:00";
    QTest::newRow("whole day") << "00:00-12:00, 12:00-24:00" << "toute la journée";
}

void TestContract::rangesRefused()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);
    QString error;
    QVERIFY(!Contract::parseRanges(text, &error));
    QVERIFY2(error.contains(expected), qPrintable(error));
}

void TestContract::nominal()
{
    const Contract base = make(Contract::Type::Base);
    QCOMPARE(base.nominal("base"), 0);
    const Contract hchp = make(Contract::Type::HcHp);
    QCOMPARE(hchp.nominal("hc"), 0);
    QCOMPARE(hchp.nominal("hp"), 100);
    const Contract ejp = make(Contract::Type::Ejp);
    QCOMPARE(ejp.nominal("hn"), 0);
    QCOMPARE(ejp.nominal("pm"), 100);
    QCOMPARE(ejp.nominal("hc"), 100); // not a period of the contract: never "cheap"

    // Relative to the prices entered, not to the names: an offer with dearer "HC" is 100 there.
    Contract::Settings settings;
    settings.type = Contract::Type::HcHp;
    settings.priceSource = Contract::PriceSource::Manual;
    settings.offPeakRanges = QStringLiteral("22:00-06:00");
    settings.manualPrices = { { "hc", 30 }, { "hp", 20 } };
    QString error;
    const Contract odd = Contract::create(settings, RegulatedTariff::builtIn(), &error).value();
    QCOMPARE(odd.nominal("hc"), 100);
    QCOMPARE(odd.nominal("hp"), 0);
    QCOMPARE(odd.periods().first(), QStringLiteral("hp"));
    // Same price for both: flat, 0.
    settings.manualPrices = { { "hc", 20 }, { "hp", 20 } };
    QCOMPARE(Contract::create(settings, RegulatedTariff::builtIn(), &error)->nominal("hp"), 0);
}

void TestContract::baseSchedule()
{
    const Contract base = make(Contract::Type::Base);
    const PriceSchedule schedule = PriceSchedule::build(base, EjpCalendar(), at("2026-11-06T10:00"), paris());
    checkShape(schedule);
    QCOMPARE(schedule.slotList().size(), 1);
    QCOMPARE(schedule.current().period, QStringLiteral("base"));
    QCOMPARE(schedule.current().nominal.value(), 0);
    QCOMPARE(schedule.current().price.value(), 20.01);
    QVERIFY(!schedule.nextSwitch().isValid());
    QCOMPARE(iso(schedule.horizonEnd()), QStringLiteral("2026-11-08T00:00:00+01:00"));
}

void TestContract::hcHpTwoRangesOneAcrossMidnight()
{
    const Contract hchp = make(Contract::Type::HcHp, QStringLiteral("12:00-14:00, 22:00-04:00"));

    PriceSchedule schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-06T10:00"), paris());
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hp"));
    QCOMPARE(schedule.current().nominal.value(), 100);
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-06T12:00:00+01:00"));
    // 04:00-12:00 hp | 12-14 hc | 14-22 hp | 22-04 hc | 04-12 hp | 12-14 hc | 14-22 hp | 22-24 hc
    QStringList expected { "hp@2026-11-06T04:00:00+01:00", "hc@2026-11-06T12:00:00+01:00", "hp@2026-11-06T14:00:00+01:00",
                           "hc@2026-11-06T22:00:00+01:00", "hp@2026-11-07T04:00:00+01:00", "hc@2026-11-07T12:00:00+01:00",
                           "hp@2026-11-07T14:00:00+01:00", "hc@2026-11-07T22:00:00+01:00" };
    QStringList actual;
    for (const PriceSchedule::Slot &slot : schedule.slotList())
        actual << slot.period + "@" + iso(slot.start);
    QCOMPARE(actual, expected);

    schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-06T13:00"), paris());
    QCOMPARE(schedule.current().period, QStringLiteral("hc"));
    QCOMPARE(schedule.current().price.value(), 15.89);
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-06T14:00:00+01:00"));

    schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-06T23:30"), paris());
    QCOMPARE(schedule.current().period, QStringLiteral("hc"));
    QCOMPARE(iso(schedule.current().start), QStringLiteral("2026-11-06T22:00:00+01:00"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-07T04:00:00+01:00"));

    // After midnight, the range started yesterday.
    schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-07T02:00"), paris());
    QCOMPARE(schedule.current().period, QStringLiteral("hc"));
    QCOMPARE(iso(schedule.current().start), QStringLiteral("2026-11-07T00:00:00+01:00"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-07T04:00:00+01:00"));
    checkShape(schedule);
}

void TestContract::hcHpDaylightSavingStart()
{
    // 2026-03-29: 02:00 CET -> 03:00 CEST. HC 22:00-06:00 lasts 7 h that night.
    const Contract hchp = make(Contract::Type::HcHp);
    const PriceSchedule schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-03-28T23:00"), paris());
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hc"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-03-29T06:00:00+02:00"));
    QCOMPARE(schedule.current().start.secsTo(schedule.current().end), 7 * 3600);
    QCOMPARE(iso(schedule.horizonEnd()), QStringLiteral("2026-03-30T00:00:00+02:00"));
}

void TestContract::hcHpDaylightSavingEnd()
{
    // 2026-10-25: 03:00 CEST -> 02:00 CET. HC 22:00-06:00 lasts 9 h that night.
    const Contract hchp = make(Contract::Type::HcHp);
    const PriceSchedule schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-10-24T23:00"), paris());
    checkShape(schedule);
    QCOMPARE(iso(schedule.current().start), QStringLiteral("2026-10-24T22:00:00+02:00"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-10-25T06:00:00+01:00"));
    QCOMPARE(schedule.current().start.secsTo(schedule.current().end), 9 * 3600);
    // Next switch in local time of the box, after the change: 22:00 CET.
    const PriceSchedule day = PriceSchedule::build(hchp, EjpCalendar(), at("2026-10-25T12:00"), paris());
    QCOMPARE(iso(day.nextSwitch()), QStringLiteral("2026-10-25T22:00:00+01:00"));
}

void TestContract::hcHpBoundaryInTheMissingHour()
{
    // HC 02:30-07:30 on the night of the change to summer time: 02:30 never happens; HC
    // starts when the wall clock jumps past it, at 03:00 CEST.
    const Contract hchp = make(Contract::Type::HcHp, QStringLiteral("02:30-07:30"));
    const PriceSchedule schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-03-29T01:00"), paris());
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hp"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-03-29T03:00:00+02:00"));
}

void TestContract::ticOverridesCurrentSlot()
{
    // The meter switched to HC at 21:58 (its clock, or ranges entered 2 min off).
    const Contract hchp = make(Contract::Type::HcHp);
    PriceSchedule schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-06T21:58"), paris(), QStringLiteral("hc"));
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hc"));
    QCOMPARE(schedule.current().source, QStringLiteral("tic"));
    QCOMPARE(schedule.current().nominal.value(), 0);
    // Same period as the ranges: only the source changes.
    schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-06T23:00"), paris(), QStringLiteral("hc"));
    QCOMPARE(schedule.current().source, QStringLiteral("tic"));
    QCOMPARE(iso(schedule.current().start), QStringLiteral("2026-11-06T22:00:00+01:00"));
    // A period that is not of the contract is ignored.
    schedule = PriceSchedule::build(hchp, EjpCalendar(), at("2026-11-06T10:00"), paris(), QStringLiteral("pm"));
    QCOMPARE(schedule.current().period, QStringLiteral("hp"));
    QCOMPARE(schedule.current().source, QStringLiteral("contrat"));
}

void TestContract::ejpNormalDayFromTic()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    // Before 07:00 normal hours say nothing of today.
    QVERIFY(!calendar.observeTic(at("2026-11-20T06:50"), TicReading::Period::EjpNormal, false));
    QCOMPARE(calendar.day(QDate(2026, 11, 20)).status, EjpCalendar::Status::Unknown);
    QVERIFY(calendar.observeTic(at("2026-11-20T07:10"), TicReading::Period::EjpNormal, false));
    QCOMPARE(calendar.day(QDate(2026, 11, 20)).status, EjpCalendar::Status::Normal);
    QCOMPARE(calendar.day(QDate(2026, 11, 20)).source, EjpCalendar::Source::Tic);

    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-20T10:00"), paris(), QStringLiteral("hn"));
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hn"));
    QCOMPARE(schedule.current().nominal.value(), 0); // a normal day stays at 0
    QCOMPARE(schedule.current().source, QStringLiteral("tic"));
    // Tomorrow from 07:00 is presumed normal: no change of period up to the horizon.
    QVERIFY(!schedule.nextSwitch().isValid());
    QCOMPARE(schedule.slotList().last().source, QStringLiteral("presume"));
}

void TestContract::ejpNoticeFromTic()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    QVERIFY(calendar.observeTic(at("2026-11-20T06:31"), TicReading::Period::EjpNormal, true));
    QCOMPARE(calendar.day(QDate(2026, 11, 20)).status, EjpCalendar::Status::Peak);
    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-20T06:35"), paris(), QStringLiteral("hn"));
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hn"));
    QCOMPARE(schedule.current().nominal.value(), 0);
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-20T07:00:00+01:00"));
    const PriceSchedule::Slot &peak = schedule.slotList().at(1);
    QCOMPARE(peak.period, QStringLiteral("pm"));
    QCOMPARE(peak.nominal.value(), 100);
    QCOMPARE(peak.price.value(), 43.24);
    QCOMPARE(peak.source, QStringLiteral("tic"));
    QCOMPARE(iso(peak.end), QStringLiteral("2026-11-21T01:00:00+01:00"));
}

void TestContract::ejpPeakFromTic()
{
    EjpCalendar calendar;
    QVERIFY(calendar.observeTic(at("2026-11-20T15:00"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.ticStatus(QDate(2026, 11, 20)), EjpCalendar::Status::Peak);
    // Normal hours seen later the same EJP day do not undo a peak.
    QVERIFY(!calendar.observeTic(at("2026-11-20T18:00"), TicReading::Period::EjpNormal, false));
    QCOMPARE(calendar.ticStatus(QDate(2026, 11, 20)), EjpCalendar::Status::Peak);
}

void TestContract::ejpPeakEndsAtOne()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    // At 00:30 the meter is still in the peak of the day before.
    QVERIFY(calendar.observeTic(at("2026-11-21T00:30"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.ticStatus(QDate(2026, 11, 20)), EjpCalendar::Status::Peak);
    QCOMPARE(calendar.ticStatus(QDate(2026, 11, 21)), EjpCalendar::Status::Unknown);

    PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-21T00:30"), paris(), QStringLiteral("pm"));
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("pm"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-21T01:00:00+01:00"));

    schedule = PriceSchedule::build(ejp, calendar, at("2026-11-21T01:00"), paris(), QStringLiteral("hn"));
    QCOMPARE(schedule.current().period, QStringLiteral("hn"));
    QCOMPARE(schedule.current().nominal.value(), 0);
    QCOMPARE(iso(schedule.current().start), QStringLiteral("2026-11-21T01:00:00+01:00"));
}

void TestContract::ejpUndeclaredDayPresumedNormal()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-20T09:00"), paris());
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hn"));
    QCOMPARE(schedule.current().source, QStringLiteral("presume"));
    QCOMPARE(schedule.current().price.value(), 17.89);
    QCOMPARE(schedule.current().nominal.value(), 0);
    for (const PriceSchedule::Slot &slot : schedule.slotList())
        QVERIFY(slot.price.has_value() && slot.nominal.has_value());
    // Presumed for prices only: the season counter keeps the day unknown.
    QCOMPARE(calendar.day(QDate(2026, 11, 20)).status, EjpCalendar::Status::Unknown);
    QCOMPARE(calendar.dayOrPresumed(QDate(2026, 11, 20)).status, EjpCalendar::Status::Normal);
    QCOMPARE(EjpCalendar::sourceCode(calendar.dayOrPresumed(QDate(2026, 11, 20)).source), QStringLiteral("presume"));
    // A declaration and the TIC both beat the presumption.
    calendar.declareNextDay(at("2026-11-20T18:00"));
    QCOMPARE(calendar.dayOrPresumed(QDate(2026, 11, 21)).status, EjpCalendar::Status::Peak);
    QCOMPARE(calendar.dayOrPresumed(QDate(2026, 11, 21)).source, EjpCalendar::Source::Manual);
    QVERIFY(calendar.observeTic(at("2026-11-22T07:10"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.dayOrPresumed(QDate(2026, 11, 22)).status, EjpCalendar::Status::Peak);
    QCOMPARE(calendar.dayOrPresumed(QDate(2026, 11, 22)).source, EjpCalendar::Source::Tic);
    // 01:00-07:00 is normal hours whatever the day.
    bool sawNight = false;
    for (const PriceSchedule::Slot &slot : schedule.slotList()) {
        if (slot.start.time() == QTime(1, 0)) {
            QCOMPARE(slot.period, QStringLiteral("hn"));
            QCOMPARE(slot.source, QStringLiteral("contrat"));
            QCOMPARE(slot.end.time(), QTime(7, 0));
            sawNight = true;
        }
    }
    QVERIFY(sawNight);
}

void TestContract::ejpOutOfSeason()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    QVERIFY(!calendar.observeTic(at("2026-04-10T09:00"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.day(QDate(2026, 4, 10)).status, EjpCalendar::Status::Normal);
    QCOMPARE(calendar.day(QDate(2026, 4, 10)).source, EjpCalendar::Source::Season);
    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-04-10T09:00"), paris());
    checkShape(schedule);
    for (const PriceSchedule::Slot &slot : schedule.slotList()) {
        QCOMPARE(slot.period, QStringLiteral("hn"));
        QVERIFY(slot.price.has_value());
    }
    QCOMPARE(schedule.current().source, QStringLiteral("saison"));
    // Season bounds.
    QVERIFY(EjpCalendar::inSeason(QDate(2026, 11, 1)));
    QVERIFY(EjpCalendar::inSeason(QDate(2027, 3, 31)));
    QVERIFY(!EjpCalendar::inSeason(QDate(2027, 4, 1)));
    QVERIFY(!EjpCalendar::inSeason(QDate(2026, 10, 31)));
}

void TestContract::ejpDeclaredThenConfirmedByNotice()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    const EjpDeclaration::Outcome outcome = EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2026-11-20T16:00"));
    QVERIFY(outcome.accepted);
    QVERIFY(outcome.changed);
    QCOMPARE(outcome.day, QDate(2026, 11, 21));
    QVERIFY(outcome.message.contains(QStringLiteral("21/11/2026 07:00 au 22/11/2026 01:00")));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Peak);
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Manual);

    // The series: tomorrow 07:00 onwards at the peak price, source "manuel".
    PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-20T16:00"), paris());
    checkShape(schedule);
    const PriceSchedule::Slot &last = schedule.slotList().last();
    QCOMPARE(iso(last.start), QStringLiteral("2026-11-21T07:00:00+01:00"));
    QCOMPARE(last.period, QStringLiteral("pm"));
    QCOMPARE(last.price.value(), 43.24);
    QCOMPARE(last.nominal.value(), 100);
    QCOMPARE(last.source, QStringLiteral("manuel"));
    QCOMPARE(schedule.toVariant(ejp).value("slots").toList().last().toMap().value("source").toString(), QStringLiteral("manuel"));

    // The scalars of the day itself, without TIC: the peak, from the declaration.
    schedule = PriceSchedule::build(ejp, calendar, at("2026-11-21T09:00"), paris());
    QCOMPARE(schedule.current().period, QStringLiteral("pm"));
    QCOMPARE(schedule.current().price.value(), 43.24);
    QCOMPARE(schedule.current().nominal.value(), 100);
    QCOMPARE(schedule.current().source, QStringLiteral("manuel"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2026-11-22T01:00:00+01:00"));

    // The notice of the meter at 06:31 confirms: the TIC is the source, no discrepancy.
    QVERIFY(calendar.observeTic(at("2026-11-21T06:31"), TicReading::Period::EjpNormal, true));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Peak);
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Tic);
    QVERIFY(calendar.discrepancy(QDate(2026, 11, 21)).isEmpty());
}

void TestContract::ejpDeclaredThenConfirmedByPeak()
{
    EjpCalendar calendar;
    calendar.declareNextDay(at("2026-11-20T16:00"));
    QVERIFY(calendar.observeTic(at("2026-11-21T07:00"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Tic);
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Peak);
    QVERIFY(calendar.discrepancy(QDate(2026, 11, 21)).isEmpty());
}

void TestContract::ejpDeclaredThenContradicted()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    calendar.declareNextDay(at("2026-11-20T18:00"));
    // Normal hours before 07:05 say nothing (clocks a few minutes apart).
    QVERIFY(!calendar.observeTic(at("2026-11-21T07:02"), TicReading::Period::EjpNormal, false));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Manual);
    QVERIFY(calendar.discrepancy(QDate(2026, 11, 21)).isEmpty());
    // HN at 07:10 on a declared day: the declaration is contradicted, the day is normal.
    QVERIFY(calendar.observeTic(at("2026-11-21T07:10"), TicReading::Period::EjpNormal, false));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Normal);
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Tic);
    QCOMPARE(calendar.discrepancy(QDate(2026, 11, 21)),
             QStringLiteral("2026-11-21 : déclaré jour EJP, la TIC lit des heures normales (déclaration contredite, jour normal)"));
    // The declaration stays recorded (it explains the discrepancy), but does not count.
    QVERIFY(calendar.isDeclared(QDate(2026, 11, 21)));
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 11, 21)), 0);

    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-21T10:00"), paris(), QStringLiteral("hn"));
    checkShape(schedule);
    QCOMPARE(schedule.current().period, QStringLiteral("hn"));
    QCOMPARE(schedule.current().nominal.value(), 0);
    QCOMPARE(schedule.current().source, QStringLiteral("tic"));
    QCOMPARE(iso(schedule.current().end), QStringLiteral("2026-11-22T01:00:00+01:00"));
}

void TestContract::ejpPeakSeenNotDeclared()
{
    EjpCalendar calendar;
    QVERIFY(calendar.observeTic(at("2026-11-21T08:00"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Peak);
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Tic);
    QVERIFY(!calendar.isDeclared(QDate(2026, 11, 21)));
    QVERIFY(calendar.discrepancy(QDate(2026, 11, 21)).isEmpty());
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 11, 21)), 1);
}

void TestContract::ejpDeclareTwice()
{
    EjpCalendar calendar;
    QVERIFY(EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2026-11-20T16:00")).changed);
    const QVariantMap before = calendar.toVariant();
    const EjpDeclaration::Outcome again = EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2026-11-20T19:00"));
    QVERIFY(again.accepted);
    QVERIFY(!again.changed);
    QCOMPARE(again.message, QStringLiteral("Le 21/11/2026 est déjà déclaré jour EJP."));
    QCOMPARE(calendar.toVariant(), before);
}

void TestContract::ejpCancelBeforeAndAfterSeven()
{
    EjpCalendar calendar;
    // Nothing declared: nothing to cancel, accepted, no change.
    EjpDeclaration::Outcome outcome = EjpDeclaration::cancelTomorrow(Contract::Type::Ejp, calendar, at("2026-11-20T18:00"));
    QVERIFY(outcome.accepted);
    QVERIFY(!outcome.changed);

    calendar.declareNextDay(at("2026-11-20T18:00"));
    // Before 07:00 of the declared day: cancelled.
    outcome = EjpDeclaration::cancelTomorrow(Contract::Type::Ejp, calendar, at("2026-11-21T06:59"));
    QVERIFY(outcome.accepted);
    QVERIFY(outcome.changed);
    QCOMPARE(outcome.day, QDate(2026, 11, 21));
    QCOMPARE(calendar.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Unknown);

    // Declared again; from 07:00 the peak has started: refused, the TIC decides.
    calendar.declareNextDay(at("2026-11-21T06:59"));
    QVERIFY(calendar.isDeclared(QDate(2026, 11, 21)));
    for (const QString &time : { QStringLiteral("2026-11-21T07:00"), QStringLiteral("2026-11-21T23:00"),
                                 QStringLiteral("2026-11-22T00:30"), QStringLiteral("2026-11-22T06:59") }) {
        outcome = EjpDeclaration::cancelTomorrow(Contract::Type::Ejp, calendar, at(time));
        QVERIFY2(!outcome.accepted, qPrintable(time));
        QVERIFY(!outcome.changed);
        QCOMPARE(outcome.day, QDate(2026, 11, 21));
        QVERIFY(outcome.message.contains(QStringLiteral("a commencé à 07:00")));
        QVERIFY(calendar.isDeclared(QDate(2026, 11, 21)));
    }
    // From 07:00 on the 22nd the declaration of the 21st is in the past: nothing to cancel.
    QVERIFY(EjpDeclaration::cancelTomorrow(Contract::Type::Ejp, calendar, at("2026-11-22T07:00")).accepted);
    QVERIFY(calendar.isDeclared(QDate(2026, 11, 21)));

    // With the 21st in progress and the 22nd declared, "cancel" cancels the 22nd.
    calendar.declareNextDay(at("2026-11-21T15:00"));
    outcome = EjpDeclaration::cancelTomorrow(Contract::Type::Ejp, calendar, at("2026-11-21T16:00"));
    QVERIFY(outcome.accepted && outcome.changed);
    QCOMPARE(outcome.day, QDate(2026, 11, 22));
    QVERIFY(calendar.isDeclared(QDate(2026, 11, 21)));
}

void TestContract::ejpDeclareBetweenMidnightAndOne()
{
    const Contract ejp = make(Contract::Type::Ejp);
    // The target is the day of the next 07:00, not the calendar day after.
    QCOMPARE(EjpCalendar::declarationTarget(at("2026-11-20T23:59")), QDate(2026, 11, 21));
    QCOMPARE(EjpCalendar::declarationTarget(at("2026-11-21T00:00")), QDate(2026, 11, 21));
    QCOMPARE(EjpCalendar::declarationTarget(at("2026-11-21T00:30")), QDate(2026, 11, 21));
    QCOMPARE(EjpCalendar::declarationTarget(at("2026-11-21T06:59")), QDate(2026, 11, 21));
    QCOMPARE(EjpCalendar::declarationTarget(at("2026-11-21T07:00")), QDate(2026, 11, 22));

    EjpCalendar calendar;
    const EjpDeclaration::Outcome outcome = EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2026-11-21T00:30"));
    QCOMPARE(outcome.day, QDate(2026, 11, 21));
    QVERIFY(!calendar.isDeclared(QDate(2026, 11, 22)));
    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-21T00:30"), paris());
    checkShape(schedule);
    bool sawPeak = false;
    for (const PriceSchedule::Slot &slot : schedule.slotList()) {
        if (slot.period == QStringLiteral("pm")) {
            QCOMPARE(iso(slot.start), QStringLiteral("2026-11-21T07:00:00+01:00"));
            QCOMPARE(iso(slot.end), QStringLiteral("2026-11-22T01:00:00+01:00"));
            QCOMPARE(slot.source, QStringLiteral("manuel"));
            sawPeak = true;
        }
    }
    QVERIFY(sawPeak);
}

void TestContract::ejpDeclareOutOfSeason()
{
    EjpCalendar calendar;
    // 31 March after 07:00: the next EJP day is 1 April, out of season.
    EjpDeclaration::Outcome outcome = EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2027-03-31T10:00"));
    QVERIFY(!outcome.accepted);
    QVERIFY(!outcome.changed);
    QCOMPARE(outcome.day, QDate(2027, 4, 1));
    QVERIFY(outcome.message.contains(QStringLiteral("hors de la saison EJP")));
    QVERIFY(!calendar.isDeclared(QDate(2027, 4, 1)));
    QVERIFY(!EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2027-06-15T10:00")).accepted);
    // 31 March before 07:00: the 31st itself, in season.
    QVERIFY(EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2027-03-31T06:00")).accepted);
    // 31 October: 1 November is in season.
    outcome = EjpDeclaration::declareTomorrow(Contract::Type::Ejp, calendar, at("2026-10-31T18:00"));
    QVERIFY(outcome.accepted);
    QCOMPARE(outcome.day, QDate(2026, 11, 1));
}

void TestContract::ejpDeclareNotEjpContract()
{
    for (Contract::Type type : { Contract::Type::Base, Contract::Type::HcHp, Contract::Type::Tempo }) {
        EjpCalendar calendar;
        EjpDeclaration::Outcome outcome = EjpDeclaration::declareTomorrow(type, calendar, at("2026-11-20T16:00"));
        QVERIFY(!outcome.accepted);
        QVERIFY(!outcome.changed);
        QVERIFY(outcome.message.contains(QStringLiteral("ne concerne que le contrat EJP")));
        QVERIFY(!calendar.isDeclared(QDate(2026, 11, 21)));
        outcome = EjpDeclaration::cancelTomorrow(type, calendar, at("2026-11-20T16:00"));
        QVERIFY(!outcome.accepted);
    }
}

void TestContract::ejpDeclarationAfterRestart()
{
    EjpCalendar calendar;
    calendar.declareNextDay(at("2026-11-20T16:00"));
    calendar.observeTic(at("2026-11-20T08:00"), TicReading::Period::EjpNormal, false);
    const QVariantMap stored = calendar.toVariant();
    QCOMPARE(stored.value("version").toInt(), 2);
    QCOMPARE(stored.value("manuel").toStringList(), QStringList { QStringLiteral("2026-11-21") });

    // Restart: the declaration comes back from the plugin storage.
    const EjpCalendar restored = EjpCalendar::fromVariant(stored);
    QVERIFY(restored.isDeclared(QDate(2026, 11, 21)));
    QCOMPARE(restored.day(QDate(2026, 11, 21)).source, EjpCalendar::Source::Manual);
    QCOMPARE(restored.day(QDate(2026, 11, 20)).source, EjpCalendar::Source::Tic);
    // ... and still cannot be cancelled once its peak started.
    EjpCalendar again = restored;
    QVERIFY(!EjpDeclaration::cancelTomorrow(Contract::Type::Ejp, again, at("2026-11-21T08:00")).accepted);

    // The storage of the previous version (TIC days only) is still read.
    const EjpCalendar old = EjpCalendar::fromVariant(QVariantMap {
        { "version", 1 }, { "tic", QVariantMap { { "2026-11-20", "pointe" } } }, { "edf", QVariantMap { { "2026-11-21", "pointe" } } } });
    QCOMPARE(old.day(QDate(2026, 11, 20)).source, EjpCalendar::Source::Tic);
    QCOMPARE(old.day(QDate(2026, 11, 21)).status, EjpCalendar::Status::Unknown);

    // A declaration of a past season is pruned.
    EjpCalendar pruned = restored;
    pruned.prune(QDate(2027, 11, 5));
    QVERIFY(!pruned.isDeclared(QDate(2026, 11, 21)));
}

void TestContract::ejpPeakOfMarch31EndsInApril()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    QVERIFY(calendar.observeTic(at("2027-03-31T08:00"), TicReading::Period::EjpMobilePeak, false));
    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2027-04-01T00:30"), paris());
    QCOMPARE(schedule.current().period, QStringLiteral("pm"));
    QCOMPARE(schedule.current().source, QStringLiteral("tic"));
    QCOMPARE(iso(schedule.nextSwitch()), QStringLiteral("2027-04-01T01:00:00+02:00"));
}

void TestContract::ejpSeasonCounterAfterRestart()
{
    EjpCalendar calendar;
    calendar.observeTic(at("2026-11-03T08:00"), TicReading::Period::EjpMobilePeak, false);
    calendar.observeTic(at("2026-11-04T08:00"), TicReading::Period::EjpMobilePeak, false);
    for (int day = 1; day <= 9; day++) {
        if (day != 3 && day != 4)
            calendar.observeTic(QDateTime(QDate(2026, 11, day), QTime(8, 0), paris()), TicReading::Period::EjpNormal, false);
    }
    const QDate today(2026, 11, 10);
    QCOMPARE(calendar.seasonPeakDays(today), 2);
    QVERIFY(calendar.seasonCountComplete(today));
    QCOMPARE(calendar.seasonUnknownDays(today), 0);

    // Restart: the counter comes back from the plugin storage, not from zero.
    const EjpCalendar restored = EjpCalendar::fromVariant(calendar.toVariant());
    QCOMPARE(restored.seasonPeakDays(today), 2);
    QVERIFY(restored.seasonCountComplete(today));
    QCOMPARE(restored.day(QDate(2026, 11, 4)).source, EjpCalendar::Source::Tic);

    // Days nobody saw (box off): the count is a lower bound, said as such.
    QCOMPARE(restored.seasonUnknownDays(QDate(2026, 11, 13)), 3);
    QVERIFY(!restored.seasonCountComplete(QDate(2026, 11, 13)));

    // An unreadable storage gives an empty calendar, not a wrong one.
    QCOMPARE(EjpCalendar::fromVariant(QVariantMap { { "version", 7 } }).seasonPeakDays(today), 0);
}

void TestContract::ejpSeasonPruned()
{
    EjpCalendar calendar;
    calendar.observeTic(at("2026-01-10T08:00"), TicReading::Period::EjpMobilePeak, false);
    calendar.observeTic(at("2026-11-02T08:00"), TicReading::Period::EjpMobilePeak, false);
    // Outside the season, the counter shows the season that ended.
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 6, 1)), 1);
    calendar.prune(QDate(2026, 11, 5));
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 11, 5)), 1);
    QCOMPARE(calendar.day(QDate(2026, 1, 10)).status, EjpCalendar::Status::Unknown);
}

void TestContract::ejpSeasonCounterWithEnteredDays()
{
    // Installed on 10 December at 15:00, during a peak: 4 peak days entered, the 10th included.
    const QDateTime installed = at("2026-12-10T15:00");
    const QDate countFrom = EjpCalendar::declarationTarget(installed);
    QCOMPARE(countFrom, QDate(2026, 12, 11));
    EjpCalendar calendar;
    calendar.setCountStart(countFrom, 4);
    QCOMPARE(calendar.seasonCountStart(QDate(2026, 12, 10)), QDate(2026, 12, 11));

    // The meter sees the peak of the 10th after the installation: in the 4, not counted again.
    QVERIFY(calendar.observeTic(at("2026-12-10T16:00"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 12, 10)), 4);
    // The days before the count are covered by the number entered: none is unknown.
    QCOMPARE(calendar.seasonUnknownDays(QDate(2026, 12, 11)), 0);
    QVERIFY(calendar.seasonCountComplete(QDate(2026, 12, 11)));

    // The 11th seen on the meter, the 12th declared, the 13th normal: 4 + 2.
    calendar.observeTic(at("2026-12-11T08:00"), TicReading::Period::EjpMobilePeak, false);
    calendar.declareNextDay(at("2026-12-11T18:00"));
    calendar.observeTic(at("2026-12-13T08:00"), TicReading::Period::EjpNormal, false);
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 12, 13)), 6);
    QVERIFY(calendar.seasonCountComplete(QDate(2026, 12, 14)));
    // A day the box did not see after the installation: the count is a lower bound.
    QCOMPARE(calendar.seasonUnknownDays(QDate(2026, 12, 15)), 1);
    QVERIFY(!calendar.seasonCountComplete(QDate(2026, 12, 15)));

    // Restart: the calendar from the storage, the count start and the number from the setup.
    EjpCalendar restored = EjpCalendar::fromVariant(calendar.toVariant());
    restored.setCountStart(countFrom, 4);
    QCOMPARE(restored.seasonPeakDays(QDate(2026, 12, 13)), 6);

    // Out of season, the counter shows the season that ended, entered days included.
    QCOMPARE(restored.seasonPeakDays(QDate(2027, 6, 1)), 6);

    // The number is bounded to the 22 days of a season.
    EjpCalendar bounded;
    bounded.setCountStart(countFrom, 30);
    QCOMPARE(bounded.seasonPeakDays(QDate(2026, 12, 10)), 22);
}

void TestContract::ejpSeasonCounterInstalledAfterMidnight()
{
    // Installed at 00:30 on the 11th: the peak of the 10th (until 01:00) has started, it is in
    // the number entered; the box counts from the 11th.
    const QDate countFrom = EjpCalendar::declarationTarget(at("2026-12-11T00:30"));
    QCOMPARE(countFrom, QDate(2026, 12, 11));
    EjpCalendar calendar;
    calendar.setCountStart(countFrom, 3);
    QVERIFY(calendar.observeTic(at("2026-12-11T00:35"), TicReading::Period::EjpMobilePeak, false));
    QCOMPARE(calendar.ticStatus(QDate(2026, 12, 10)), EjpCalendar::Status::Peak);
    QCOMPARE(calendar.seasonPeakDays(QDate(2026, 12, 11)), 3);

    // Installed at 05:00 on the 11th: the peak of the 11th has not started, the box counts it.
    EjpCalendar early;
    early.setCountStart(EjpCalendar::declarationTarget(at("2026-12-11T05:00")), 3);
    early.observeTic(at("2026-12-11T08:00"), TicReading::Period::EjpMobilePeak, false);
    QCOMPARE(early.seasonPeakDays(QDate(2026, 12, 11)), 4);
}

void TestContract::ejpSeasonCounterCountsFromSeven()
{
    // The plugin publishes the counter on the EJP day in progress (ejpDayOf(now)): a day
    // declared at 04:31, or announced at 06:30, is not counted before its peak starts at 07:00.
    EjpCalendar calendar;
    calendar.setCountStart(QDate(2026, 11, 2), 0);
    QDate target;
    QCOMPARE(calendar.declareNextDay(at("2026-11-07T04:31"), &target), EjpCalendar::DeclareResult::Declared);
    QCOMPARE(target, QDate(2026, 11, 7));
    QCOMPARE(calendar.seasonPeakDays(EjpCalendar::ejpDayOf(at("2026-11-07T04:31"))), 0);
    QVERIFY(calendar.observeTic(at("2026-11-07T06:30"), TicReading::Period::EjpNormal, true));
    QCOMPARE(calendar.seasonPeakDays(EjpCalendar::ejpDayOf(at("2026-11-07T06:30"))), 0);
    // At 07:00 the meter reads PM: the day is already known (notice), nothing changes but the day in progress.
    calendar.observeTic(at("2026-11-07T07:00"), TicReading::Period::EjpMobilePeak, false);
    QCOMPARE(calendar.day(QDate(2026, 11, 7)).status, EjpCalendar::Status::Peak);
    QCOMPARE(calendar.seasonPeakDays(EjpCalendar::ejpDayOf(at("2026-11-07T07:00"))), 1);
    // Until 07:00 the next day the EJP day in progress is still the 7th: counted once.
    QCOMPARE(calendar.seasonPeakDays(EjpCalendar::ejpDayOf(at("2026-11-08T06:59"))), 1);
}

void TestContract::ejpSeasonEnteredDaysOfAnotherSeason()
{
    EjpCalendar calendar;
    calendar.setCountStart(QDate(2026, 12, 11), 4);
    calendar.observeTic(at("2027-11-05T08:00"), TicReading::Period::EjpMobilePeak, false);
    // The next season starts from zero: the number entered was for the season of 2026-2027.
    QCOMPARE(calendar.seasonCountStart(QDate(2027, 11, 6)), QDate(2027, 11, 1));
    QCOMPARE(calendar.seasonPeakDays(QDate(2027, 11, 6)), 1);

    // Installed on 1 November before 07:00: no day of the season has passed, a number
    // entered by mistake does not apply.
    EjpCalendar first;
    first.setCountStart(EjpCalendar::declarationTarget(at("2026-11-01T05:00")), 2);
    QCOMPARE(first.seasonPeakDays(QDate(2026, 11, 3)), 0);
    // Installed on 1 November at 08:00: the 1st may already be a peak day, the number applies.
    EjpCalendar second;
    second.setCountStart(EjpCalendar::declarationTarget(at("2026-11-01T08:00")), 1);
    QCOMPARE(second.seasonPeakDays(QDate(2026, 11, 3)), 1);
}

void TestContract::ejpTicOutsideWindowsIgnored()
{
    EjpCalendar calendar;
    // A peak at 03:00 does not exist: ignored, not attributed to a day.
    QVERIFY(!calendar.observeTic(at("2026-11-20T03:00"), TicReading::Period::EjpMobilePeak, false));
    // A notice outside 06:00-07:30 neither.
    QVERIFY(!calendar.observeTic(at("2026-11-20T12:00"), TicReading::Period::EjpNormal, true) && calendar.ticStatus(QDate(2026, 11, 20)) != EjpCalendar::Status::Peak);
    // HN at 00:30 tells about the day before.
    QVERIFY(calendar.observeTic(at("2026-11-21T00:30"), TicReading::Period::EjpNormal, false));
    QCOMPARE(calendar.ticStatus(QDate(2026, 11, 20)), EjpCalendar::Status::Normal);
}

void TestContract::transportJson()
{
    const Contract ejp = make(Contract::Type::Ejp);
    EjpCalendar calendar;
    calendar.observeTic(at("2026-11-20T07:30"), TicReading::Period::EjpNormal, false);
    const PriceSchedule schedule = PriceSchedule::build(ejp, calendar, at("2026-11-20T10:00"), paris(), QStringLiteral("hn"));
    const QVariantMap map = schedule.toVariant(ejp);
    QCOMPARE(map.value("schema").toString(), QStringLiteral("etm.edf.priceSlots"));
    QCOMPARE(map.value("version").toInt(), 1);
    QCOMPARE(map.value("timeZone").toString(), QStringLiteral("Europe/Paris"));
    QCOMPARE(map.value("contract").toString(), QStringLiteral("ejp"));
    QCOMPARE(map.value("priceSource").toString(), QStringLiteral("regulated"));
    QCOMPARE(map.value("horizonEnd").toString(), QStringLiteral("2026-11-22T00:00:00+01:00"));
    QCOMPARE(map.value("periods").toMap().value("pm").toMap().value("nominal").toInt(), 100);

    const QJsonDocument document = QJsonDocument::fromVariant(map);
    const QJsonArray slotArray = document.object().value("slots").toArray();
    QVERIFY(slotArray.size() >= 3);
    const QJsonObject first = slotArray.first().toObject();
    QCOMPARE(first.value("start").toString(), QStringLiteral("2026-11-20T07:00:00+01:00"));
    QCOMPARE(first.value("price").toDouble(), 17.89);
    QCOMPARE(first.value("nominal").toInt(), 0);
    QCOMPARE(first.value("source").toString(), QStringLiteral("tic"));
    const QJsonObject last = slotArray.last().toObject();
    QCOMPARE(last.value("period").toString(), QStringLiteral("hn"));
    QCOMPARE(last.value("price").toDouble(), 17.89);
    QCOMPARE(last.value("nominal").toInt(), 0);
    QCOMPARE(last.value("source").toString(), QStringLiteral("presume"));
}

void TestContract::subscribedPowerFromIsousc_data()
{
    QTest::addColumn<int>("entered");
    QTest::addColumn<int>("isousc");
    QTest::addColumn<bool>("threePhase");
    QTest::addColumn<QString>("discrepancy");
    QTest::newRow("mono 30 A = 6 kVA") << 6 << 30 << false << QString();
    QTest::newRow("mono 45 A = 9 kVA") << 9 << 45 << false << QString();
    QTest::newRow("mono 60 A = 12 kVA") << 12 << 60 << false << QString();
    QTest::newRow("mono 15 A = 3 kVA") << 3 << 15 << false << QString();
    QTest::newRow("tri 15 A per phase = 9 kVA") << 9 << 15 << true << QString();
    QTest::newRow("tri 20 A per phase = 12 kVA") << 12 << 20 << true << QString();
    QTest::newRow("tri 60 A per phase = 36 kVA") << 36 << 60 << true << QString();
    QTest::newRow("no ISOUSC") << 6 << 0 << false << QString();
    QTest::newRow("mono 45 A, 6 entered") << 6 << 45 << false
        << QStringLiteral("ISOUSC 45 A (monophasé) = 9 kVA, puissance souscrite saisie 6 kVA (rien n'est recalé)");
    QTest::newRow("tri 20 A, 9 entered") << 9 << 20 << true
        << QStringLiteral("ISOUSC 20 A par phase (triphasé) = 12 kVA, puissance souscrite saisie 9 kVA (rien n'est recalé)");
    // Read as single phase, 15 A would be 3 kVA: the phase count matters.
    QTest::newRow("tri 15 A read as mono") << 9 << 15 << false
        << QStringLiteral("ISOUSC 15 A (monophasé) = 3 kVA, puissance souscrite saisie 9 kVA (rien n'est recalé)");
}

void TestContract::subscribedPowerFromIsousc()
{
    QFETCH(int, entered);
    QFETCH(int, isousc);
    QFETCH(bool, threePhase);
    QFETCH(QString, discrepancy);
    QCOMPARE(Contract::subscribedPowerDiscrepancy(entered, isousc, threePhase), discrepancy);
}

QTEST_GUILESS_MAIN(TestContract)
#include "tst_contract.moc"
