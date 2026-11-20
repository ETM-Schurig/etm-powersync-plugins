// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef CONTRACT_H
#define CONTRACT_H

#include "regulatedtariff.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>
#include <optional>

// An electricity contract: its option, its periods and the price of each, deterministic.
//
// Periods use the historic TIC vocabulary: "base"; "hc" / "hp"; "hn" / "pm" (EJP normal hours /
// mobile peak). Tempo is reserved: refused for now.
class Contract
{
public:
    enum class Type { Base, HcHp, Ejp, Tempo };
    enum class PriceSource { Regulated, Manual };

    // A wall-clock range of the box [from, to). from > to crosses midnight; to == 00:00 is 24:00.
    struct Range {
        QTime from;
        QTime to;
        bool contains(const QTime &time) const;
        int minutes() const;
        bool operator==(const Range &other) const { return from == other.from && to == other.to; }
    };

    struct Settings {
        Type type = Type::Base;
        PriceSource priceSource = PriceSource::Regulated;
        int subscribedKva = 6;
        QString offPeakRanges;            // "22:00-06:00" or "12:00-14:00, 22:00-04:00"
        QHash<QString, double> manualPrices; // period -> c€/kWh, used with PriceSource::Manual
    };

    // Builds the contract, or says in clear why not (error, for the user).
    static std::optional<Contract> create(const Settings &settings, const RegulatedTariff &tariff, QString *error);

    // "22:00-06:00, 12:00-14:00": HH:MM-HH:MM separated by "," or ";". Empty is an error.
    static std::optional<QList<Range>> parseRanges(const QString &text, QString *error);

    static QString typeCode(Type type);      // "base", "hchp", "ejp", "tempo"
    static std::optional<Type> typeFromCode(const QString &code);

    Type type() const { return m_type; }
    PriceSource priceSource() const { return m_priceSource; }
    int subscribedKva() const { return m_subscribedKva; }
    QList<Range> offPeakRanges() const { return m_ranges; }

    // The periods of the contract, cheapest first.
    QStringList periods() const { return m_periods; }
    double price(const QString &period) const { return m_prices.value(period); }
    bool hasPeriod(const QString &period) const { return m_prices.contains(period); }

    // Nominal price 0-100 of a period, relative to the prices of THIS contract (min and max of
    // its periods, never of the day): 0 = the cheapest period, 100 = the dearest. A contract
    // whose periods all cost the same (Base) is 0 everywhere. Same convention as the "rank" of
    // the tariff interface: lower is better.
    int nominal(const QString &period) const;

    // The subscribed power implied by ISOUSC (historic TIC), in kVA. Enedis counts 5 A per kVA
    // (200 V): single phase, 30 A = 6 kVA, 45 A = 9 kVA, 60 A = 12 kVA. Three phase, ISOUSC is
    // the current of ONE phase: 15 A = 9 kVA, 20 A = 12 kVA, 60 A = 36 kVA (x 3).
    static double kvaFromSubscribedCurrent(int amps, bool threePhase);
    // "" when ISOUSC agrees with the power entered, otherwise both, in clear. Nothing is
    // corrected: the power entered stays the one used for the price.
    static QString subscribedPowerDiscrepancy(int enteredKva, int subscribedA, bool threePhase);

    // HC/HP only: the period of a wall-clock time from the off-peak ranges.
    QString hcHpPeriodAt(const QTime &wallTime) const;

    // Periods of an EJP day: normal hours, and the mobile peak from 07:00 to 01:00 the day after.
    static QTime ejpPeakStart() { return QTime(7, 0); }
    static QTime ejpPeakEnd() { return QTime(1, 0); }

private:
    Type m_type = Type::Base;
    PriceSource m_priceSource = PriceSource::Regulated;
    int m_subscribedKva = 6;
    QList<Range> m_ranges;
    QStringList m_periods;
    QHash<QString, double> m_prices;
};

#endif // CONTRACT_H
