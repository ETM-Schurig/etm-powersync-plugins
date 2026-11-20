// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "priceschedule.h"

#include <QSet>
#include <QVariantList>
#include <algorithm>

namespace {

QString iso(const QDateTime &dateTime)
{
    return dateTime.toString(Qt::ISODate);
}

// The instant a wall-clock time of a day happens. A time inside the gap of the change to
// summer time (02:30 on the last Sunday of March in France) does not happen: the boundary is
// the first instant after the gap (03:00), when the wall clock is already past that time.
QDateTime wallInstant(const QDate &day, const QTime &time, const QTimeZone &zone)
{
    const QDateTime dateTime(day, time, zone);
    if (dateTime.time() == time && dateTime.date() == day)
        return dateTime;
    const QTimeZone::OffsetData transition = zone.previousTransition(dateTime.addSecs(1));
    if (transition.atUtc.isValid())
        return transition.atUtc.toTimeZone(zone);
    return dateTime;
}

}

PriceSchedule PriceSchedule::build(const Contract &contract, const EjpCalendar &calendar,
                                   const QDateTime &now, const QTimeZone &zone, const QString &ticPeriod)
{
    PriceSchedule schedule;
    schedule.m_zone = zone;
    schedule.m_now = now.toTimeZone(zone);
    const QDate today = schedule.m_now.date();
    const QDateTime horizonStart(today, QTime(0, 0), zone);
    schedule.m_horizonEnd = QDateTime(today.addDays(2), QTime(0, 0), zone);

    // Every instant where the period may change, in absolute time. Wall-clock boundaries are
    // placed day by day in the box time zone, so a 23 h or 25 h day falls right.
    QSet<qint64> boundaries { horizonStart.toMSecsSinceEpoch(), schedule.m_horizonEnd.toMSecsSinceEpoch() };
    QList<QTime> wallBoundaries;
    if (contract.type() == Contract::Type::HcHp) {
        for (const Contract::Range &range : contract.offPeakRanges())
            wallBoundaries << range.from << range.to;
    } else if (contract.type() == Contract::Type::Ejp) {
        wallBoundaries << Contract::ejpPeakStart() << Contract::ejpPeakEnd();
    }
    for (QDate day = today.addDays(-1); day <= today.addDays(2); day = day.addDays(1)) {
        for (const QTime &time : wallBoundaries) {
            const qint64 at = wallInstant(day, time, zone).toMSecsSinceEpoch();
            if (at > horizonStart.toMSecsSinceEpoch() && at < schedule.m_horizonEnd.toMSecsSinceEpoch())
                boundaries.insert(at);
        }
    }
    QList<qint64> sorted(boundaries.begin(), boundaries.end());
    std::sort(sorted.begin(), sorted.end());

    QList<Slot> raw;
    for (int i = 0; i + 1 < sorted.size(); i++) {
        Slot slot;
        slot.start = QDateTime::fromMSecsSinceEpoch(sorted.at(i), zone);
        slot.end = QDateTime::fromMSecsSinceEpoch(sorted.at(i + 1), zone);
        const QTime wall = slot.start.time();
        switch (contract.type()) {
        case Contract::Type::Base:
        case Contract::Type::Tempo:
            slot.period = QStringLiteral("base");
            slot.source = QStringLiteral("contrat");
            break;
        case Contract::Type::HcHp:
            slot.period = contract.hcHpPeriodAt(wall);
            slot.source = QStringLiteral("contrat");
            break;
        case Contract::Type::Ejp:
            if (wall >= Contract::ejpPeakEnd() && wall < Contract::ejpPeakStart()) {
                slot.period = QStringLiteral("hn");
                slot.source = QStringLiteral("contrat");
            } else {
                const EjpCalendar::Day day = calendar.dayOrPresumed(EjpCalendar::ejpDayOf(slot.start));
                slot.period = day.status == EjpCalendar::Status::Peak ? QStringLiteral("pm") : QStringLiteral("hn");
                slot.source = EjpCalendar::sourceCode(day.source);
            }
            break;
        }
        raw.append(slot);
    }

    // The meter says what the period is now: it wins over the contract for the slot in
    // progress, from now on.
    if (!ticPeriod.isEmpty() && contract.hasPeriod(ticPeriod)) {
        for (int i = 0; i < raw.size(); i++) {
            Slot &slot = raw[i];
            if (!(slot.start <= schedule.m_now && schedule.m_now < slot.end))
                continue;
            if (slot.period == ticPeriod) {
                slot.source = QStringLiteral("tic");
            } else {
                Slot fromNow = slot;
                fromNow.start = schedule.m_now;
                fromNow.period = ticPeriod;
                fromNow.source = QStringLiteral("tic");
                slot.end = schedule.m_now;
                raw.insert(i + 1, fromNow);
            }
            break;
        }
    }

    for (const Slot &slot : raw) {
        if (slot.end <= schedule.m_now || slot.start >= slot.end)
            continue;
        if (!schedule.m_slots.isEmpty() && schedule.m_slots.last().period == slot.period
                && schedule.m_slots.last().source == slot.source && schedule.m_slots.last().end == slot.start) {
            schedule.m_slots.last().end = slot.end;
            continue;
        }
        schedule.m_slots.append(slot);
    }
    for (Slot &slot : schedule.m_slots) {
        if (contract.hasPeriod(slot.period)) {
            slot.price = contract.price(slot.period);
            slot.nominal = contract.nominal(slot.period);
        }
    }
    return schedule;
}

const PriceSchedule::Slot &PriceSchedule::current() const
{
    for (const Slot &slot : m_slots) {
        if (slot.start <= m_now && m_now < slot.end)
            return slot;
    }
    return m_slots.first();
}

QDateTime PriceSchedule::nextSwitch() const
{
    const QString period = current().period;
    for (const Slot &slot : m_slots) {
        if (slot.start > m_now && slot.period != period)
            return slot.start;
    }
    return QDateTime();
}

QVariantMap PriceSchedule::toVariant(const Contract &contract) const
{
    QVariantMap periods;
    for (const QString &period : contract.periods()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("price"), contract.price(period));
        entry.insert(QStringLiteral("nominal"), contract.nominal(period));
        periods.insert(period, entry);
    }

    QVariantList slotList;
    for (const Slot &slot : m_slots) {
        QVariantMap entry;
        entry.insert(QStringLiteral("start"), iso(slot.start));
        entry.insert(QStringLiteral("end"), iso(slot.end));
        entry.insert(QStringLiteral("period"), slot.period);
        entry.insert(QStringLiteral("price"), slot.price ? QVariant(*slot.price) : QVariant::fromValue(nullptr));
        entry.insert(QStringLiteral("nominal"), slot.nominal ? QVariant(*slot.nominal) : QVariant::fromValue(nullptr));
        entry.insert(QStringLiteral("source"), slot.source);
        slotList.append(entry);
    }

    QVariantMap map;
    map.insert(QStringLiteral("schema"), QStringLiteral("etm.edf.priceSlots"));
    map.insert(QStringLiteral("version"), TransportVersion);
    map.insert(QStringLiteral("generatedAt"), iso(m_now));
    map.insert(QStringLiteral("timeZone"), QString::fromUtf8(m_zone.id()));
    map.insert(QStringLiteral("contract"), Contract::typeCode(contract.type()));
    map.insert(QStringLiteral("priceSource"), contract.priceSource() == Contract::PriceSource::Regulated
               ? QStringLiteral("regulated") : QStringLiteral("manual"));
    map.insert(QStringLiteral("unit"), QStringLiteral("c€/kWh TTC"));
    map.insert(QStringLiteral("periods"), periods);
    map.insert(QStringLiteral("horizonEnd"), iso(m_horizonEnd));
    map.insert(QStringLiteral("slots"), slotList);
    return map;
}
