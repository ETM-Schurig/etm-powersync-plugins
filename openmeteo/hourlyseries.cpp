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

#include "hourlyseries.h"
#include "pvcurve.h"

#include <QHash>

namespace HourlySeries {

QVariantList atInstants(const QVariantList &times, const QVariantList &values, uint first,
                        int count, Kind kind)
{
    QHash<uint, int> index;
    if (values.count() == times.count()) {
        for (int i = 0; i < times.count(); i++)
            index.insert(times.at(i).toUInt(), i);
    }
    QVariantList out;
    for (int slot = 0; slot < count; slot++) {
        const int i = index.value(first + slot * StepS, -1);
        if (i < 0 || values.at(i).isNull())
            out.append(QVariant());
        else if (kind == Int)
            out.append(values.at(i).toInt());
        else
            out.append(values.at(i).toDouble());
    }
    return out;
}

namespace {

bool allNull(const QVariantList &values)
{
    for (const QVariant &v : values) {
        if (!v.isNull())
            return false;
    }
    return true;
}

}

QVariantMap observedGti(const QVariantMap &reply)
{
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    const QVariantList gtis = hourly.value("global_tilted_irradiance").toList();
    if (times.isEmpty() || gtis.count() != times.count())
        return QVariantMap();

    // The reply starts at yesterday 00:00 local; that first sample is the mean of the hour
    // before it and falls outside the window. The last slot (today 23:00-24:00) is never
    // in a reply of the same day - it is the future - and comes as yesterday's slot 23
    // in tomorrow's reply: two days of overlap, so no hour is lost to the latency.
    const uint t0 = PvCurve::localDayStart(times.first().toUInt(), reply.value("utc_offset_seconds").toInt());
    const QVariantList gti = atInstants(times, gtis, t0 + StepS, Slots, Double);
    if (allNull(gti))
        return QVariantMap();

    QVariantMap series;
    series.insert("v", 1);
    series.insert("t0", t0);
    series.insert("step", StepS);
    series.insert("unit", "W/m2");
    series.insert("convention", "mean over [t, t+step)");
    series.insert("regime", "observed");
    series.insert("source", "satellite_radiation_seamless");
    series.insert("gti", gti);
    return series;
}

QVariantMap siteWeather(const QVariantMap &reply)
{
    const QVariantMap hourly = reply.value("hourly").toMap();
    const QVariantList times = hourly.value("time").toList();
    if (times.isEmpty())
        return QVariantMap();

    // Anchored on the day of the reply's current step, like the transport's t0.
    const uint now = reply.value("current").toMap().value("time").toUInt();
    const uint t0 = PvCurve::localDayStart(now, reply.value("utc_offset_seconds").toInt());

    // Absent is null, never 0, and here it matters most: WMO code 0 is a clear sky and
    // is_day 0 is night. A missing hour drawn as 0 would show a sun, without an error.
    const QVariantList weatherCode = atInstants(times, hourly.value("weather_code").toList(), t0, Slots, Int);
    const QVariantList isDay = atInstants(times, hourly.value("is_day").toList(), t0, Slots, Int);
    const QVariantList tempC = atInstants(times, hourly.value("temperature_2m").toList(), t0, Slots, Double);
    if (allNull(weatherCode) && allNull(isDay) && allNull(tempC))
        return QVariantMap();

    QVariantMap convention;
    convention.insert("weatherCode", "WMO code, instantaneous at t");
    convention.insert("isDay", "1 day, 0 night, instantaneous at t");
    convention.insert("tempC", "temperature_2m in C, instantaneous at t");

    QVariantMap series;
    series.insert("v", 1);
    series.insert("t0", t0);
    series.insert("step", StepS);
    series.insert("regime", "forecast");
    series.insert("convention", convention);
    series.insert("weatherCode", weatherCode);
    series.insert("isDay", isDay);
    series.insert("tempC", tempC);
    return series;
}

}
