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

#ifndef HOURLYSERIES_H
#define HOURLYSERIES_H

#include <QVariantList>
#include <QVariantMap>

// Hourly series published as one versioned state each, on the model of
// forecastCurveTransport (CONTRACT_A sec. 5): absolute instants, slot i covering
// [t0 + i*step, t0 + (i+1)*step), absent = null and never 0.
//
// Kept free of nymea, like PvCurve, and tested from recorded replies.
namespace HourlySeries {

static const int StepS = 3600;
static const int Slots = 48;

enum Kind { Double, Int };

// count values, element i read at the instant first + i*step, looked up by INSTANT in
// times - never by index. null where the instant is missing, the value is null, or the
// series is not as long as times (then it cannot be placed on those instants).
QVariantList atInstants(const QVariantList &times, const QVariantList &values, uint first,
                        int count, Kind kind);

// observedGtiSeries, v1 (CONTRACT_A sec. 1.7), from the plane's satellite reply
// (past_days=1, forecast_days=1). Window: yesterday 00:00 local to today 24:00, 48 slots.
// Slot i reads the sample at t0 + (i+1)*step, the mean of the hour that ENDS there - the
// convention of the transport, so gti[i] is the observation of the slot whose start is
// targetAt = t0 + i*step, and the slot of observedGtiTime is observedGtiTime - 3600.
// Empty when the reply carries no non-null sample in the window: a failure.
QVariantMap observedGti(const QVariantMap &reply);

// forecastHourlyWeather, v1 (CONTRACT_A sec. 1.8), from the site's forecast reply
// (forecast_days=2). Window: today 00:00 local to tomorrow 24:00, 48 slots. The three
// variables are INSTANTANEOUS at their timestamp (Open-Meteo docs: "Instant"; measured
// for temperature_2m against minutely_15, and for is_day against sunrise and sunset), so
// slot i reads the sample at its own start, t0 + i*step - unlike the irradiance series,
// whose sample at T is the mean of the hour before. model and issuedAt are added by the
// caller. Empty when none of the three variables carries a value: a failure.
QVariantMap siteWeather(const QVariantMap &reply);

}

#endif // HOURLYSERIES_H
