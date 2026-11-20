// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "contract.h"

#include <QBitArray>
#include <QLocale>
#include <QRegularExpression>
#include <cmath>

namespace {

int minuteOfDay(const QTime &time)
{
    return time.hour() * 60 + time.minute();
}

QString periodName(const QString &period)
{
    static const QHash<QString, QString> names {
        { QStringLiteral("base"), QStringLiteral("Base") },
        { QStringLiteral("hc"), QStringLiteral("heures creuses") },
        { QStringLiteral("hp"), QStringLiteral("heures pleines") },
        { QStringLiteral("hn"), QStringLiteral("heures normales (EJP)") },
        { QStringLiteral("pm"), QStringLiteral("pointe mobile (EJP)") },
    };
    return names.value(period, period);
}

}

bool Contract::Range::contains(const QTime &time) const
{
    const int t = minuteOfDay(time);
    const int a = minuteOfDay(from);
    const int b = to == QTime(0, 0) ? 1440 : minuteOfDay(to);
    if (a < b)
        return t >= a && t < b;
    return t >= a || t < b; // crosses midnight
}

int Contract::Range::minutes() const
{
    const int a = minuteOfDay(from);
    const int b = to == QTime(0, 0) ? 1440 : minuteOfDay(to);
    return a < b ? b - a : 1440 - a + b;
}

QString Contract::typeCode(Type type)
{
    switch (type) {
    case Type::Base: return QStringLiteral("base");
    case Type::HcHp: return QStringLiteral("hchp");
    case Type::Ejp: return QStringLiteral("ejp");
    case Type::Tempo: return QStringLiteral("tempo");
    }
    return QString();
}

std::optional<Contract::Type> Contract::typeFromCode(const QString &code)
{
    for (Type type : { Type::Base, Type::HcHp, Type::Ejp, Type::Tempo }) {
        if (typeCode(type) == code)
            return type;
    }
    return std::nullopt;
}

std::optional<QList<Contract::Range>> Contract::parseRanges(const QString &text, QString *error)
{
    static const QRegularExpression rangeExpression(QStringLiteral("^\\s*(\\d{1,2}):(\\d{2})\\s*-\\s*(\\d{1,2}):(\\d{2})\\s*$"));
    QList<Range> ranges;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part.trimmed().isEmpty())
            continue;
        const QRegularExpressionMatch match = rangeExpression.match(part);
        if (!match.hasMatch()) {
            *error = QStringLiteral("Plage d'heures creuses illisible : « %1 » (attendu HH:MM-HH:MM, par exemple 22:00-06:00).").arg(part.trimmed());
            return std::nullopt;
        }
        const int h1 = match.captured(1).toInt(), m1 = match.captured(2).toInt();
        int h2 = match.captured(3).toInt();
        const int m2 = match.captured(4).toInt();
        if (h2 == 24 && m2 == 0)
            h2 = 0;
        const QTime from(h1, m1), to(h2, m2);
        if (!from.isValid() || !to.isValid() || h1 == 24) {
            *error = QStringLiteral("Heure invalide dans la plage « %1 ».").arg(part.trimmed());
            return std::nullopt;
        }
        if (from == to) {
            *error = QStringLiteral("La plage « %1 » est vide ou couvre 24 h : début et fin doivent différer.").arg(part.trimmed());
            return std::nullopt;
        }
        ranges.append(Range { from, to });
    }
    if (ranges.isEmpty()) {
        *error = QStringLiteral("Saisissez les plages d'heures creuses du compteur (par exemple 22:00-06:00).");
        return std::nullopt;
    }
    // No overlap: a minute belongs to one range at most.
    QBitArray used(1440);
    for (int i = 0; i < ranges.size(); i++) {
        for (int minute = 0; minute < 1440; minute++) {
            if (!ranges.at(i).contains(QTime(minute / 60, minute % 60)))
                continue;
            if (used.testBit(minute)) {
                *error = QStringLiteral("Les plages d'heures creuses se chevauchent à %1.")
                        .arg(QTime(minute / 60, minute % 60).toString(QStringLiteral("HH:mm")));
                return std::nullopt;
            }
            used.setBit(minute);
        }
    }
    if (used.count(true) == 1440) {
        *error = QStringLiteral("Les plages d'heures creuses couvrent toute la journée : ce n'est pas un contrat HC/HP.");
        return std::nullopt;
    }
    return ranges;
}

std::optional<Contract> Contract::create(const Settings &settings, const RegulatedTariff &tariff, QString *error)
{
    Contract contract;
    contract.m_type = settings.type;
    contract.m_priceSource = settings.priceSource;
    contract.m_subscribedKva = settings.subscribedKva;

    switch (settings.type) {
    case Type::Tempo:
        *error = QStringLiteral("L'option Tempo n'est pas encore prise en charge par ce plugin. "
                                "Choisissez Base, Heures creuses ou EJP ; Tempo viendra dans une version suivante.");
        return std::nullopt;
    case Type::Base:
        contract.m_periods = { QStringLiteral("base") };
        break;
    case Type::HcHp: {
        const std::optional<QList<Range>> ranges = parseRanges(settings.offPeakRanges, error);
        if (!ranges)
            return std::nullopt;
        contract.m_ranges = *ranges;
        contract.m_periods = { QStringLiteral("hc"), QStringLiteral("hp") };
        break;
    }
    case Type::Ejp:
        contract.m_periods = { QStringLiteral("hn"), QStringLiteral("pm") };
        break;
    }

    const QString option = typeCode(settings.type);
    for (const QString &period : contract.m_periods) {
        if (settings.priceSource == PriceSource::Regulated) {
            const std::optional<double> price = tariff.price(option, period, settings.subscribedKva);
            if (!price) {
                *error = QStringLiteral("Le barème réglementé fourni n'a pas de prix pour %1 à %2 kVA : choisissez « prix saisis ».")
                        .arg(periodName(period)).arg(settings.subscribedKva);
                return std::nullopt;
            }
            contract.m_prices.insert(period, *price);
        } else {
            const double price = settings.manualPrices.value(period, 0);
            // 0 is the default of an empty field: a price that was not entered, not a free kWh.
            if (!(price > 0) || !std::isfinite(price) || price > 1000) {
                *error = QStringLiteral("Saisissez le prix %1 en c€/kWh TTC (par exemple 20.01).").arg(periodName(period));
                return std::nullopt;
            }
            contract.m_prices.insert(period, price);
        }
    }
    // Cheapest first (for EJP and HC/HP it is the usual order, but entered prices decide).
    std::stable_sort(contract.m_periods.begin(), contract.m_periods.end(), [&contract](const QString &a, const QString &b) {
        return contract.m_prices.value(a) < contract.m_prices.value(b);
    });
    return contract;
}

int Contract::nominal(const QString &period) const
{
    if (!m_prices.contains(period))
        return 100;
    double low = m_prices.value(m_periods.first());
    double high = low;
    for (double price : m_prices) {
        low = qMin(low, price);
        high = qMax(high, price);
    }
    if (high - low < 1e-9)
        return 0;
    return static_cast<int>(std::lround(100.0 * (m_prices.value(period) - low) / (high - low)));
}

QString Contract::hcHpPeriodAt(const QTime &wallTime) const
{
    for (const Range &range : m_ranges) {
        if (range.contains(wallTime))
            return QStringLiteral("hc");
    }
    return QStringLiteral("hp");
}

double Contract::kvaFromSubscribedCurrent(int amps, bool threePhase)
{
    return amps * 0.2 * (threePhase ? 3 : 1);
}

QString Contract::subscribedPowerDiscrepancy(int enteredKva, int subscribedA, bool threePhase)
{
    if (subscribedA <= 0)
        return QString();
    const double kva = kvaFromSubscribedCurrent(subscribedA, threePhase);
    if (qAbs(kva - enteredKva) < 0.5)
        return QString();
    return QStringLiteral("ISOUSC %1 A%2 = %3 kVA, puissance souscrite saisie %4 kVA (rien n'est recalé)")
            .arg(subscribedA)
            .arg(threePhase ? QStringLiteral(" par phase (triphasé)") : QStringLiteral(" (monophasé)"))
            .arg(QLocale(QLocale::French).toString(kva, 'g', 3))
            .arg(enteredKva);
}
