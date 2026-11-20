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

#ifndef PVCURVE_H
#define PVCURVE_H

#include <QVariantList>
#include <QVariantMap>

// The forecastCurveTransport payload, built from one hourly API reply (CONTRACT_A sec. 5).
//
// Kept free of nymea on purpose: it is the only place where the published curve is
// computed, and it is tested from recorded replies without a network or a nymead.
namespace PvCurve {

static const int TransportVersion = 2;
static const int Slots = 48;
static const int StepS = 3600;

// Declared by the installer on the plane thing, never learned (CONTRACT_A sec. 2.1).
struct Plane {
    double kwp = 0;
    double performanceRatio = 1.0;
    double tempCoeffPctPerK = -0.37;
    double noctC = 45;
};

// NOCT cell temperature model: Tcell = T2m + GTI * (NOCT - 20) / 800.
double cellTemperature(double gti, double t2m, double noctC);

// Power factor at that cell temperature, relative to STC (25 C). Above 1 in the cold.
double temperatureFactor(double gti, double t2m, double tempCoeffPctPerK, double noctC);

// Power of one sample [W], before rounding: the expression of p, corrected or not. The
// curve and the scalar states both go through here, so they cannot drift apart.
double samplePower(double gti, double t2m, const Plane &plane, bool corrected);

// The decision of the run: true when every one of the 48 slots from t0 has a temperature,
// read by instant. The curve and the scalar states are corrected together or not at all.
bool temperatureComplete(const QVariantList &times, const QVariantList &temperature, uint t0);

// Fills the hourly part of the v2 payload: v, t0, step, unit, convention, regime,
// corrections, p, gti, tempC. Provenance (model, issuedAt) is added by the caller.
//
// Slot i reads the API sample timestamped t0 + (i+1)*step, looked up by INSTANT in
// times - never by index. Returns false when a GTI sample is missing in the window: no
// curve is published then, as in v1.
//
// The temperature correction applies to the whole run or not at all. One missing
// temperature sample and the run is published uncorrected, corrections = [], with p
// equal to the v1 value bit for bit - a curve half corrected would carry an effect that
// its corrections list cannot name.
bool build(const QVariantList &times, const QVariantList &gti, const QVariantList &temperature,
           uint t0, const Plane &plane, QVariantMap &curve);

// Local calendar day start, on the utc_offset_seconds the API reports for the site.
uint localDayStart(uint unixTime, int utcOffsetS);

// Hourly values are the mean over the PRECEDING hour: the first bucket entirely in the
// future is the first one strictly after "now" (CONTRACT_A sec. 2.2).
int firstForwardBucket(const QVariantList &times, uint now);

// The scalar states of a plane for one plane reply (CONTRACT_A sec. 1.1-1.3), keyed
// gtiSource, pvPower, energyNext3h, energyNext6h, energyNext12h, energyRestOfDay,
// energyTomorrow, peakTime. A key that is absent is not published this cycle.
//
// corrected must be the decision of the run (temperatureComplete): every hourly energy
// is then the sum of the same rounded per-slot powers as p, and pvPower is corrected with
// current.temperature_2m - or left out when that one is missing, rather than published
// uncorrected beside corrected energies. sunset 0 = unknown: no rest-of-day.
QVariantMap scalars(const QVariantMap &reply, uint todayStart, uint sunset, const Plane &plane,
                    bool corrected);

}

#endif // PVCURVE_H
