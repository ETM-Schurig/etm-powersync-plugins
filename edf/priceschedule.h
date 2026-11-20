// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef PRICESCHEDULE_H
#define PRICESCHEDULE_H

#include "contract.h"
#include "ejpcalendar.h"

#include <QDateTime>
#include <QList>
#include <QTimeZone>
#include <QVariantMap>
#include <optional>

// The price slots of a contract from now to the end of tomorrow (box local time): a
// deterministic computation from the contract, its off-peak ranges, the EJP calendar and,
// for the slot in progress, the period read on the meter.
//
// Slot sources:
//   "contrat"  the period comes from the contract (single price, off-peak ranges, EJP normal
//              hours 01:00-07:00)
//   "tic"      the period in progress, as read on the meter (PTEC)
//   "manuel"   EJP 07:00-01:00 of a day declared by the user: peak
//   "saison"   EJP outside the season: normal
//   "presume"  EJP 07:00-01:00 of a day in season neither read nor declared: presumed normal
class PriceSchedule
{
public:
    struct Slot {
        QDateTime start;           // box local time zone
        QDateTime end;
        QString period;            // "base", "hc", "hp", "hn", "pm", or "unknown"
        std::optional<double> price;
        std::optional<int> nominal;
        QString source;
    };

    // ticPeriod: the period read on a linked meter now ("" = none). It overrides the period of
    // the contract for the slot in progress, from now to the end of that slot.
    static PriceSchedule build(const Contract &contract, const EjpCalendar &calendar,
                               const QDateTime &now, const QTimeZone &zone,
                               const QString &ticPeriod = QString());

    const QList<Slot> &slotList() const { return m_slots; }
    QDateTime now() const { return m_now; }
    QDateTime horizonEnd() const { return m_horizonEnd; }

    // The slot containing now (always exists).
    const Slot &current() const;
    // The next time the period changes after now, or invalid if not within the horizon.
    QDateTime nextSwitch() const;

    // The JSON transport of the slots: schema "etm.edf.priceSlots", version 1.
    QVariantMap toVariant(const Contract &contract) const;

    static constexpr int TransportVersion = 1;

private:
    QList<Slot> m_slots;
    QDateTime m_now;
    QDateTime m_horizonEnd;
    QTimeZone m_zone;
};

#endif // PRICESCHEDULE_H
