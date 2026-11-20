// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef REGULATEDTARIFF_H
#define REGULATEDTARIFF_H

#include <QDate>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <optional>

// The regulated tariff (Tarif Bleu EDF) shipped with the package: tariffs/trv-edf.json,
// compiled in. It changes with the versions of the package, never from the network.
class RegulatedTariff
{
public:
    // The one compiled in (resource :/edf/tariffs/trv-edf.json).
    static const RegulatedTariff &builtIn();
    static RegulatedTariff fromJson(const QByteArray &json, QString *error = nullptr);

    bool isValid() const { return m_valid; }
    QDate effectiveDate() const { return m_effectiveDate; }
    QString name() const { return m_name; }
    QString source() const { return m_source; }
    bool toVerify() const { return m_toVerify; }

    // c€/kWh TTC. option: "base", "hchp", "ejp", "tempo"; period: "base", "hc", "hp", "hn", "pm", ...
    // Base depends on the subscribed power (kVA).
    std::optional<double> price(const QString &option, const QString &period, int subscribedKva) const;

private:
    bool m_valid = false;
    QDate m_effectiveDate;
    QString m_name;
    QString m_source;
    bool m_toVerify = true;
    QJsonObject m_options;
};

#endif // REGULATEDTARIFF_H
