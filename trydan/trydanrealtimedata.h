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

#ifndef TRYDANREALTIMEDATA_H
#define TRYDANREALTIMEDATA_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantList>

#include <optional>

// One reply of GET /RealTimeData, and what the plugin derives from it.
//
// Every field is optional: firmware versions differ in what they send (the per-phase
// IntensityMeasure_* / VoltageMeasure_* fields and ChargeMode are not in every version),
// and a field the charger did not send must not turn into a zero that looks like a
// measurement. Presence is read from the JSON itself.
//
// Kept free of nymea and of the network, so that every rule below is tested offline.
struct TrydanRealTimeData
{
    QString id;
    QString firmwareVersion;

    std::optional<int> chargeState;      // 0 = no vehicle, 1 = vehicle connected, 2 = charging
    std::optional<double> chargePower;   // W
    std::optional<double> chargeEnergy;  // kWh, reset by the charger when a charge stops
    std::optional<int> slaveError;
    std::optional<int> paused;           // 1 = paused
    std::optional<int> locked;           // 1 = locked
    std::optional<int> intensity;        // A, the current setpoint
    std::optional<int> minIntensity;     // A
    std::optional<int> maxIntensity;     // A
    std::optional<int> dynamic;          // 1 = the charger's own power control is configured
    std::optional<int> pauseDynamic;     // 1 = that control is suspended
    std::optional<int> chargeMode;       // 0 = single phase, 1 = three phase, 2 = mixed
    std::optional<double> intensityMeasure[3]; // A, per phase
    std::optional<double> voltageMeasure[3];   // V, per phase

    // Parses one reply. Returns false (and leaves *data untouched) when the body is not a
    // JSON object. A JSON object without the Trydan fields parses, but see looksLikeTrydan().
    static bool parse(const QByteArray &body, TrydanRealTimeData *data, QString *error = nullptr);

    // The fingerprint used by discovery and by setup: the fields every Trydan firmware
    // sends and that are needed to drive it. A reply without them is not a Trydan, or not
    // one this plugin can drive.
    bool looksLikeTrydan() const;

    // Reads the integer value of a writable parameter by its /write/ name. Used to check
    // that a write was taken. Unknown names, or a field the charger did not send, give
    // std::nullopt.
    std::optional<int> writableValue(const QString &param) const;
};

namespace TrydanModel {

// Below this charging power the charger is not delivering energy, whatever ChargeState
// says (it stays at 2 after Paused=1). A Trydan at its 6 A minimum on one phase draws
// about 1.4 kW, so 100 W keeps a margin over standby and rounding noise.
constexpr double ChargingPowerThresholdW = 100.0;

// Below this per-phase current a phase is not in use. Published as exactly 0, because the
// energy manager counts a phase as metered when its current is non-zero. Far below the
// 6 A minimum of IEC 61851 and above induced or rounding noise on an open phase.
constexpr double ActivePhaseThresholdA = 1.0;

// IEC 61851: no setpoint below 6 A.
constexpr int MinimumCurrentA = 6;

enum class ChargeMode { Unknown, Single, Three, Mixed };

ChargeMode chargeMode(const TrydanRealTimeData &data);
QString chargeModeName(ChargeMode mode);

// ChargeState 1 or 2, or a current flowing (see currentFlowing()): no current flows
// without a vehicle. Any other value (or none) keeps the previous value: an unknown state
// is not an unplug.
bool pluggedIn(const TrydanRealTimeData &data, bool previous);

// Plugged in AND drawing power, the power being publishedPower(). Never ChargeState
// alone: it stays at 2 after Paused=1. True whenever currentFlowing().
bool charging(const TrydanRealTimeData &data);

// True when the firmware sends the per-phase fields and they carry values. Voltages are
// tested because a grid-connected charger always sees mains voltage on L1: all-zero (or
// absent) voltages mean the fields are not populated.
bool phaseMeasurementsAvailable(const TrydanRealTimeData &data);

// Phase currents with the dead band applied: below ActivePhaseThresholdA, exactly 0.
// All three are 0 when the measurements are not available.
QList<double> phaseCurrents(const TrydanRealTimeData &data);

// At least one phase current at or above ActivePhaseThresholdA.
bool currentFlowing(const TrydanRealTimeData &data);

// Nominal mains voltage (EN 50160), for a phase that carries current but whose voltage
// does not read as mains (below PhasePresentV): a measured current is not dropped
// because its voltage is missing.
constexpr double NominalVoltageV = 230.0;

// Sum over the phases of phaseCurrents() times the phase voltage. std::nullopt without
// the per-phase measurements.
std::optional<double> measuredPower(const TrydanRealTimeData &data);

// ChargePower agrees with measuredPower() when both are on the same side of
// ChargingPowerThresholdW and differ by at most the larger of PowerAgreementW and
// PowerAgreementRatio of the larger of the two. In steady charge the two differ by a few
// percent (power factor, voltage); in a transition ChargePower jumps ahead of the
// measured currents - to the new setpoint, or to 0 at a pause - by far more.
constexpr double PowerAgreementRatio = 0.2;
constexpr double PowerAgreementW = 300.0;

enum class PowerSource { None, ChargePower, PhaseMeasurements };

struct PublishedPower {
    double watts = 0;
    PowerSource source = PowerSource::None; // None: nothing to publish, keep the state
};

// The power published as currentPower, from the same reading as the phase currents.
//  - With the per-phase measurements: ChargePower while it agrees with measuredPower(),
//    otherwise (absent, or contradicting the currents) measuredPower(). The currents are
//    what the charger measures on each phase; ChargePower is its own figure, and in a
//    transition it says something the currents do not.
//  - Without them: ChargePower, the only figure there is.
PublishedPower publishedPower(const TrydanRealTimeData &data);

// The wiring of the charger, a setup parameter chosen by the installer: the charger does
// not report it on its local API. Stored as the text of the choice; read from its
// beginning, so these prefixes must stay in the allowed values of the JSON.
// std::nullopt while nothing was chosen (the first allowed value, or a charger added
// before the parameter existed).
inline const QString WiringSinglePrefix = QStringLiteral("Single phase");
inline const QString WiringThreePrefix = QStringLiteral("Three phase");
std::optional<uint> wiringFromParam(const QString &value);

// Mains voltage seen on a phase, and none. In between, no verdict.
constexpr double PhasePresentV = 150.0;
constexpr double PhaseAbsentV = 50.0;

enum class WiringCheck { NotChecked, Consistent, VoltageOnL2L3, NoVoltageOnL2L3 };
QString wiringCheckName(WiringCheck check);

// The wiring set against the voltages measured on L2 and L3. Information only: it
// changes nothing that is driven.
//  - NotChecked: no per-phase measurements, L1 not at mains voltage, or a voltage on
//    L2/L3 between PhaseAbsentV and PhasePresentV.
//  - VoltageOnL2L3: set as single phase, but L2 or L3 carries mains voltage.
//  - NoVoltageOnL2L3: set as three phase, but L2 or L3 carries none, while the charger
//    is set to draw on three phases (ChargeMode three phase, or a firmware without
//    ChargeMode). In single-phase or mixed mode the charger leaves L2/L3 open itself:
//    their absence is then NotChecked, their presence still Consistent.
// The wiring is the installer's parameter, the mode the charger's reading: one is never
// read for the other.
WiringCheck wiringCheck(const TrydanRealTimeData &data, uint wiringPhases);

// Phases actually in use, for the energy manager's phaseCount.
//  - While charging, with phase measurements: the number of phases with current.
//  - Otherwise: what the charger is set to draw, capped by the wiring (single phase
//    mode -> 1; three phase or mixed or unknown mode -> the wiring).
// Never 0, never above 3. A measurement above the wiring is published as measured: the
// wiring setting is then wrong, and the measurement is the fact.
uint phaseCount(const TrydanRealTimeData &data, uint wiringPhases);

struct DesiredPhases {
    QVariantList possibleValues; // {1} or {3} when the charger cannot switch, {1, 3} when it can
    std::optional<uint> value;   // std::nullopt: keep the published value (mixed mode)
    bool canSwitch = false;
};

// desiredPhaseCount, read back from ChargeMode.
//  - Single-phase wiring: frozen at 1, possible values {1}. Three phases are never
//    announced on a single-phase charger, whatever ChargeMode reads.
//  - Three-phase wiring, ChargeMode sent: {1, 3}; 0 -> 1, 1 -> 3, mixed -> unchanged.
//  - Three-phase wiring, no ChargeMode (firmware without phase switching): frozen at 3.
DesiredPhases desiredPhases(const TrydanRealTimeData &data, uint wiringPhases);

// The ChargeMode value for a phase count: 1 -> 0, 3 -> 1.
int chargeModeFor(uint phases);

struct CurrentRange {
    int min = MinimumCurrentA;
    int max = MinimumCurrentA;
};

// [max(6, MinIntensity), max(min, MaxIntensity)]. The charger may be stricter than
// IEC 61851, never looser; a range read inverted closes on a point instead.
CurrentRange currentRange(const TrydanRealTimeData &data);

struct CurrentCommand {
    enum Kind { Nothing, Pause, SetIntensity };
    Kind kind = Nothing;
    int ampere = 0; // SetIntensity only
};

// The maxChargingCurrent gesture, decided on a reading.
//  - Below the minimum of currentRange(): the charger does not work there, so charging
//    stops - Paused=1, nothing if already paused. Never set to the minimum instead: that
//    would draw more than asked. Intensity is not written.
//  - From the minimum up: Intensity, rounded down (never more than asked) and capped at
//    the maximum; nothing if the reading already shows it. The pause is left as it is:
//    resuming is the power gesture, not a side effect of a setpoint.
CurrentCommand currentCommand(const TrydanRealTimeData &data, double requested);

}

#endif // TRYDANREALTIMEDATA_H
