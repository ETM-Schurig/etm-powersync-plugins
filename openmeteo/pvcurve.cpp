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

#include "pvcurve.h"

#include <QtGlobal>

namespace PvCurve {

namespace {

// CONTRACT_A sec. 1.4: no more precision than sec. 0.1 allows to claim.
double roundTo(double value, double quantum)
{
    return qRound(value / quantum) * quantum;
}

int indexOf(const QVariantList &times, uint instant)
{
    for (int i = 0; i < times.count(); i++) {
        if (times.at(i).toUInt() == instant)
            return i;
    }
    return -1;
}

// Sum of hourly powers [W] over [from, to) indices -> Wh. False if any is missing: a short
// sum would read as a low forecast (CONTRACT_A: fresh data or none).
bool sumWh(const QVariantList &power, int from, int to, double &wh)
{
    if (from < 0 || to > power.count() || from >= to)
        return false;
    double sum = 0;
    for (int i = from; i < to; i++) {
        if (power.at(i).isNull())
            return false;
        sum += power.at(i).toDouble();
    }
    wh = sum;
    return true;
}

}

double cellTemperature(double gti, double t2m, double noctC)
{
    return t2m + gti * (noctC - 20.0) / 800.0;
}

double temperatureFactor(double gti, double t2m, double tempCoeffPctPerK, double noctC)
{
    return 1.0 + tempCoeffPctPerK / 100.0 * (cellTemperature(gti, t2m, noctC) - 25.0);
}

double samplePower(double gti, double t2m, const Plane &plane, bool corrected)
{
    if (!corrected) {
        // The v1 expression, unchanged: an uncorrected v2 curve equals v1 bit for bit.
        return gti * plane.kwp * plane.performanceRatio;
    }
    // No AC limit here: clipping applies to what the inverter receives from all its
    // planes, so it is a site parameter applied by ems after the sum.
    const double factor = temperatureFactor(gti, t2m, plane.tempCoeffPctPerK, plane.noctC);
    return qMax(0.0, gti * plane.kwp * plane.performanceRatio * factor);
}

bool temperatureComplete(const QVariantList &times, const QVariantList &temperature, uint t0)
{
    // All hourly series of a reply share the one "time" array. A temperature series of
    // another length cannot be placed on those instants, so it counts as absent.
    if (temperature.count() != times.count())
        return false;
    for (int slot = 0; slot < Slots; slot++) {
        const int index = indexOf(times, t0 + (slot + 1) * StepS);
        if (index < 0 || temperature.at(index).isNull())
            return false;
    }
    return true;
}

bool build(const QVariantList &times, const QVariantList &gti, const QVariantList &temperature,
           uint t0, const Plane &plane, QVariantMap &curve)
{
    curve.clear();
    if (gti.count() != times.count())
        return false;

    const bool temperatureAligned = temperature.count() == times.count();
    const bool corrected = temperatureComplete(times, temperature, t0);

    QVariantList gtis;
    QVariantList temperatures;
    for (int slot = 0; slot < Slots; slot++) {
        const int index = indexOf(times, t0 + (slot + 1) * StepS);
        if (index < 0 || gti.at(index).isNull())
            return false;
        gtis.append(gti.at(index).toDouble());

        // Absence is null, never 0: 0 C is a temperature.
        if (temperatureAligned && !temperature.at(index).isNull())
            temperatures.append(temperature.at(index).toDouble());
        else
            temperatures.append(QVariant());
    }

    QVariantList points;
    for (int slot = 0; slot < Slots; slot++)
        points.append(qRound(samplePower(gtis.at(slot).toDouble(), temperatures.at(slot).toDouble(),
                                         plane, corrected)));

    QVariantList corrections;
    if (corrected)
        corrections.append(QStringLiteral("temperature"));

    curve.insert("v", TransportVersion);
    curve.insert("t0", t0);
    curve.insert("step", StepS);
    curve.insert("unit", "W");
    curve.insert("convention", "mean over [t, t+step)");
    curve.insert("regime", "forecast");
    curve.insert("corrections", corrections);
    curve.insert("p", points);
    curve.insert("gti", gtis);
    curve.insert("tempC", temperatures);
    return true;
}

uint localDayStart(uint unixTime, int utcOffsetS)
{
    const qint64 shifted = static_cast<qint64>(unixTime) + utcOffsetS;
    return static_cast<uint>((shifted / 86400) * 86400 - utcOffsetS);
}

int firstForwardBucket(const QVariantList &times, uint now)
{
    for (int i = 0; i < times.count(); i++) {
        if (times.at(i).toUInt() > now)
            return i;
    }
    return -1;
}

QVariantMap scalars(const QVariantMap &reply, uint todayStart, uint sunset, const Plane &plane,
                    bool corrected)
{
    QVariantMap out;
    const QVariantMap current = reply.value("current").toMap();
    const QVariantMap hourly = reply.value("hourly").toMap();

    // "current" is the instant (15 min model step); it drives pvPower. P[W] = GTI * kWp * PR,
    // times the temperature factor when the run is corrected. Ignores shading, soiling and
    // inverter clipping - all systematic, all optimistic (CONTRACT_A sec. 0.1).
    if (current.contains("global_tilted_irradiance")) {
        const double gti = current.value("global_tilted_irradiance").toDouble();
        out.insert("gtiSource", gti);
        const QVariant t2m = current.value("temperature_2m");
        if (!corrected)
            out.insert("pvPower", roundTo(samplePower(gti, 0, plane, false), 50.0));
        else if (!t2m.isNull())
            out.insert("pvPower", roundTo(samplePower(gti, t2m.toDouble(), plane, true), 50.0));
    }

    const QVariantList times = hourly.value("time").toList();
    const QVariantList gtis = hourly.value("global_tilted_irradiance").toList();
    if (times.isEmpty() || gtis.count() != times.count())
        return out;
    const QVariantList temps = hourly.value("temperature_2m").toList();
    const bool temperatureAligned = temps.count() == times.count();

    // Every hourly energy sums the same rounded per-sample powers as p, so the energy of
    // a window equals the sum of the published p over it, to the 0.1 kWh rounding.
    QVariantList power;
    for (int i = 0; i < times.count(); i++) {
        if (gtis.at(i).isNull() || (corrected && (!temperatureAligned || temps.at(i).isNull())))
            power.append(QVariant());
        else
            power.append(qRound(samplePower(gtis.at(i).toDouble(),
                                            corrected ? temps.at(i).toDouble() : 0, plane, corrected)));
    }

    const uint now = current.value("time").toUInt();
    const int start = firstForwardBucket(times, now);
    if (start < 0)
        return out;

    // Rolling windows. These cross midnight by construction: at 23:00 all of the next
    // 12 h is tomorrow morning. That is their definition, not a defect - the state that
    // answers "what is left today" is energyRestOfDay, below.
    const struct { int hours; const char *key; } windows[] = {
        { 3,  "energyNext3h" },
        { 6,  "energyNext6h" },
        { 12, "energyNext12h" },
    };
    for (const auto &window : windows) {
        double wh = 0;
        if (sumWh(power, start, start + window.hours, wh))
            out.insert(window.key, roundTo(wh / 1000.0, 0.1));
    }

    // Bounded by sunset, not by is_day. is_day is an instantaneous hourly flag: it reads
    // 0 at 07:00 for a 07:17 sunrise, which would drop the 43 min of daylight in the
    // 07:00-08:00 bucket. It qualifies an hour; it never bounds an integral.
    if (sunset > now) {
        int end = start;
        // Include the bucket that contains sunset: it starts before sunset.
        while (end < times.count() && times.at(end).toUInt() - 3600 < sunset)
            end++;
        double wh = 0;
        if (sumWh(power, start, end, wh))
            out.insert("energyRestOfDay", roundTo(wh / 1000.0, 0.1));
    }

    const uint tomorrowStart = todayStart + 86400;
    int from = -1, to = -1;
    for (int i = 0; i < times.count(); i++) {
        const uint t = times.at(i).toUInt();
        if (t > tomorrowStart && t <= tomorrowStart + 86400) {
            if (from < 0)
                from = i;
            to = i + 1;
        }
    }
    double wh = 0;
    if (from >= 0 && sumWh(power, from, to, wh))
        out.insert("energyTomorrow", roundTo(wh / 1000.0, 0.1));

    // Peak of today's forecast IRRADIANCE (the state is named so), not of the power.
    double best = -1;
    uint bestTime = 0;
    for (int i = 0; i < times.count(); i++) {
        const uint t = times.at(i).toUInt();
        if (t <= todayStart || t > todayStart + 86400 || gtis.at(i).isNull())
            continue;
        if (gtis.at(i).toDouble() > best) {
            best = gtis.at(i).toDouble();
            bestTime = t;
        }
    }
    if (bestTime > 0)
        out.insert("peakTime", bestTime);

    return out;
}

}
