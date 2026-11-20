// SPDX-License-Identifier: GPL-3.0-or-later

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
* Copyright (C) 2026, ETM-Schurig SARL
*
* This file is part of etm-powersync-plugins.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <https://www.gnu.org/licenses/>.
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "ensemblespread.h"
#include "pvcurve.h"

#include <QStringList>
#include <QtMath>

namespace EnsembleSpread {

namespace {

// Ensemble spread is computed on GTI, not GHI. Measured on 2026-09-08: CV(GTI) exceeds
// CV(GHI) by 2.0 to 4.6 points, systematically, and the gap widens as spread grows.
// Using GHI would understate the dispersion of the quantity actually published, and it
// would understate it in the optimistic direction.
bool coefficientOfVariation(const QList<double> &samples, double &cvPercent)
{
    if (samples.count() < 5)
        return false;
    double sum = 0;
    for (double s : samples)
        sum += s;
    const double mean = sum / samples.count();
    if (mean <= 0)
        return false;
    double sq = 0;
    for (double s : samples)
        sq += (s - mean) * (s - mean);
    cvPercent = 100.0 * qSqrt(sq / samples.count()) / mean;
    return true;
}

}

QVariantList daily(const QVariantMap &reply)
{
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    if (times.isEmpty())
        return QVariantList();
    const uint todayStart = PvCurve::localDayStart(times.first().toUInt(), reply.value("utc_offset_seconds").toInt());

    QStringList members;
    for (auto it = hourly.constBegin(); it != hourly.constEnd(); ++it) {
        if (it.key().startsWith("global_tilted_irradiance_member"))
            members.append(it.key());
    }
    if (members.isEmpty())
        return QVariantList();

    QVariantList spread;
    for (int day = 0; day < 2; day++) {
        // Hourly values are preceding-hour means: the day is the samples in (from, to].
        const uint from = todayStart + day * 86400;
        const uint to = from + 86400;
        QList<double> totals;
        for (const QString &member : members) {
            const QVariantList values = hourly.value(member).toList();
            if (values.count() != times.count())
                continue;
            double sum = 0;
            bool complete = true;
            for (int i = 0; i < times.count(); i++) {
                const uint t = times.at(i).toUInt();
                if (t <= from || t > to)
                    continue;
                if (values.at(i).isNull()) {
                    complete = false;
                    break;
                }
                sum += values.at(i).toDouble();
            }
            if (complete && sum > 0)
                totals.append(sum);
        }
        double cv = 0;
        if (coefficientOfVariation(totals, cv))
            spread.append(qRound(cv / 0.1) * 0.1);
        else
            spread.append(QVariant());
    }
    return spread;
}

bool fresh(qint64 spreadTime, qint64 now)
{
    return spreadTime > 0 && now - spreadTime <= MaxAgeS;
}

double invalid()
{
    return qQNaN();
}

}
