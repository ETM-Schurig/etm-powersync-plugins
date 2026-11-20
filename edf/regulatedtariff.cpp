// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "regulatedtariff.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

static void initEdfResources()
{
    // The resource lives in the plugin (or the test binary) that compiles edf.qrc.
    Q_INIT_RESOURCE(edf);
}

const RegulatedTariff &RegulatedTariff::builtIn()
{
    static const RegulatedTariff tariff = []() {
        initEdfResources();
        QFile file(QStringLiteral(":/edf/tariffs/trv-edf.json"));
        if (!file.open(QIODevice::ReadOnly))
            return RegulatedTariff();
        return fromJson(file.readAll());
    }();
    return tariff;
}

RegulatedTariff RegulatedTariff::fromJson(const QByteArray &json, QString *error)
{
    RegulatedTariff tariff;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || root.value(QStringLiteral("schema")).toString() != QStringLiteral("etm.edf.regulatedTariff")
            || root.value(QStringLiteral("version")).toInt() != 1) {
        if (error)
            *error = QStringLiteral("not a regulated tariff file of version 1");
        return tariff;
    }
    tariff.m_effectiveDate = QDate::fromString(root.value(QStringLiteral("effectiveDate")).toString(), Qt::ISODate);
    tariff.m_name = root.value(QStringLiteral("name")).toString();
    tariff.m_source = root.value(QStringLiteral("source")).toString();
    // Absent means "to verify": the flag is cleared explicitly, never by omission.
    tariff.m_toVerify = root.value(QStringLiteral("toVerify")).toBool(true);
    tariff.m_options = root.value(QStringLiteral("options")).toObject();
    tariff.m_valid = tariff.m_effectiveDate.isValid() && !tariff.m_options.isEmpty();
    if (!tariff.m_valid && error)
        *error = QStringLiteral("effective date or options missing");
    return tariff;
}

std::optional<double> RegulatedTariff::price(const QString &option, const QString &period, int subscribedKva) const
{
    if (!m_valid)
        return std::nullopt;
    const QJsonObject entry = m_options.value(option).toObject();
    if (option == QStringLiteral("base")) {
        if (period != QStringLiteral("base"))
            return std::nullopt;
        for (const QJsonValue &value : entry.value(QStringLiteral("bySubscribedPower")).toArray()) {
            const QJsonObject band = value.toObject();
            const int minKva = band.value(QStringLiteral("minKva")).toInt(0);
            const int maxKva = band.value(QStringLiteral("maxKva")).toInt(1000);
            if (subscribedKva >= minKva && subscribedKva <= maxKva && band.value(QStringLiteral("price")).isDouble())
                return band.value(QStringLiteral("price")).toDouble();
        }
        return std::nullopt;
    }
    const QJsonValue value = entry.value(QStringLiteral("periods")).toObject().value(period);
    if (!value.isDouble())
        return std::nullopt;
    return value.toDouble();
}
