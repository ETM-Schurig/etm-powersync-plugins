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

#include "trydanrealtimedata.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtMath>

namespace {

// Numbers are expected as JSON numbers. A numeric string is accepted as well: it costs
// nothing and a firmware that quotes its numbers would otherwise read as "field absent".
std::optional<double> readDouble(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

std::optional<int> readInt(const QJsonObject &object, const QString &key)
{
    const std::optional<double> value = readDouble(object, key);
    if (!value || !qIsFinite(*value))
        return std::nullopt;
    return qRound(*value);
}

QString readString(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble());
    return QString();
}

}

bool TrydanRealTimeData::parse(const QByteArray &body, TrydanRealTimeData *data, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                                  : QStringLiteral("not a JSON object");
        return false;
    }

    const QJsonObject o = document.object();
    TrydanRealTimeData d;
    d.id = readString(o, QStringLiteral("ID"));
    d.firmwareVersion = readString(o, QStringLiteral("FirmwareVersion"));
    d.chargeState = readInt(o, QStringLiteral("ChargeState"));
    d.chargePower = readDouble(o, QStringLiteral("ChargePower"));
    d.chargeEnergy = readDouble(o, QStringLiteral("ChargeEnergy"));
    d.slaveError = readInt(o, QStringLiteral("SlaveError"));
    d.paused = readInt(o, QStringLiteral("Paused"));
    d.locked = readInt(o, QStringLiteral("Locked"));
    d.intensity = readInt(o, QStringLiteral("Intensity"));
    d.minIntensity = readInt(o, QStringLiteral("MinIntensity"));
    d.maxIntensity = readInt(o, QStringLiteral("MaxIntensity"));
    d.dynamic = readInt(o, QStringLiteral("Dynamic"));
    d.pauseDynamic = readInt(o, QStringLiteral("PauseDynamic"));
    d.chargeMode = readInt(o, QStringLiteral("ChargeMode"));
    for (int i = 0; i < 3; ++i) {
        d.intensityMeasure[i] = readDouble(o, QStringLiteral("IntensityMeasure_L%1").arg(i + 1));
        d.voltageMeasure[i] = readDouble(o, QStringLiteral("VoltageMeasure_L%1").arg(i + 1));
    }

    *data = d;
    return true;
}

bool TrydanRealTimeData::looksLikeTrydan() const
{
    return chargeState && chargePower && paused && intensity && maxIntensity;
}

std::optional<int> TrydanRealTimeData::writableValue(const QString &param) const
{
    if (param == QLatin1String("Paused"))
        return paused;
    if (param == QLatin1String("Intensity"))
        return intensity;
    if (param == QLatin1String("ChargeMode"))
        return chargeMode;
    if (param == QLatin1String("PauseDynamic"))
        return pauseDynamic;
    if (param == QLatin1String("Locked"))
        return locked;
    return std::nullopt;
}

namespace TrydanModel {

ChargeMode chargeMode(const TrydanRealTimeData &data)
{
    if (!data.chargeMode)
        return ChargeMode::Unknown;
    switch (*data.chargeMode) {
    case 0: return ChargeMode::Single;
    case 1: return ChargeMode::Three;
    case 2: return ChargeMode::Mixed;
    default: return ChargeMode::Unknown;
    }
}

QString chargeModeName(ChargeMode mode)
{
    switch (mode) {
    case ChargeMode::Single: return QStringLiteral("Single phase");
    case ChargeMode::Three: return QStringLiteral("Three phase");
    case ChargeMode::Mixed: return QStringLiteral("Mixed");
    case ChargeMode::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

bool pluggedIn(const TrydanRealTimeData &data, bool previous)
{
    if (currentFlowing(data))
        return true;
    if (!data.chargeState)
        return previous;
    switch (*data.chargeState) {
    case 0: return false;
    case 1:
    case 2: return true;
    default: return previous;
    }
}

bool charging(const TrydanRealTimeData &data)
{
    const bool vehicle = currentFlowing(data)
            || (data.chargeState && (*data.chargeState == 1 || *data.chargeState == 2));
    // A current flowing gives at least ActivePhaseThresholdA x PhasePresentV, above the
    // threshold: publishedPower() never keeps a ChargePower under it while current flows.
    return vehicle && publishedPower(data).watts >= ChargingPowerThresholdW;
}

bool phaseMeasurementsAvailable(const TrydanRealTimeData &data)
{
    double voltageSum = 0;
    for (int i = 0; i < 3; ++i) {
        if (!data.intensityMeasure[i] || !data.voltageMeasure[i])
            return false;
        voltageSum += qAbs(*data.voltageMeasure[i]);
    }
    return voltageSum > 0;
}

QList<double> phaseCurrents(const TrydanRealTimeData &data)
{
    QList<double> currents{0, 0, 0};
    if (!phaseMeasurementsAvailable(data))
        return currents;
    for (int i = 0; i < 3; ++i) {
        const double a = *data.intensityMeasure[i];
        currents[i] = qAbs(a) >= ActivePhaseThresholdA ? a : 0.0;
    }
    return currents;
}

bool currentFlowing(const TrydanRealTimeData &data)
{
    for (double a : phaseCurrents(data)) {
        if (a != 0.0)
            return true;
    }
    return false;
}

std::optional<double> measuredPower(const TrydanRealTimeData &data)
{
    if (!phaseMeasurementsAvailable(data))
        return std::nullopt;
    const QList<double> currents = phaseCurrents(data);
    double watts = 0;
    for (int i = 0; i < 3; ++i) {
        const double v = qAbs(*data.voltageMeasure[i]);
        watts += qAbs(currents.at(i)) * (v >= PhasePresentV ? v : NominalVoltageV);
    }
    return watts;
}

PublishedPower publishedPower(const TrydanRealTimeData &data)
{
    PublishedPower power;
    const std::optional<double> measured = measuredPower(data);
    if (!measured) {
        if (data.chargePower) {
            power.watts = *data.chargePower;
            power.source = PowerSource::ChargePower;
        }
        return power;
    }
    if (data.chargePower) {
        const double reported = *data.chargePower;
        const bool sameSide = (reported >= ChargingPowerThresholdW) == currentFlowing(data);
        const double tolerance = qMax(PowerAgreementW, PowerAgreementRatio * qMax(qAbs(reported), *measured));
        // Written so that a NaN does not agree.
        if (sameSide && qAbs(reported - *measured) <= tolerance) {
            power.watts = reported;
            power.source = PowerSource::ChargePower;
            return power;
        }
    }
    power.watts = *measured;
    power.source = PowerSource::PhaseMeasurements;
    return power;
}

std::optional<uint> wiringFromParam(const QString &value)
{
    const QString v = value.trimmed();
    if (v.startsWith(WiringSinglePrefix))
        return 1u;
    if (v.startsWith(WiringThreePrefix))
        return 3u;
    return std::nullopt;
}

QString wiringCheckName(WiringCheck check)
{
    switch (check) {
    case WiringCheck::Consistent: return QStringLiteral("Consistent");
    case WiringCheck::VoltageOnL2L3: return QStringLiteral("Voltage on L2/L3 but set as single phase");
    case WiringCheck::NoVoltageOnL2L3: return QStringLiteral("No voltage on L2/L3 but set as three phase");
    case WiringCheck::NotChecked: break;
    }
    return QStringLiteral("Not checked");
}

WiringCheck wiringCheck(const TrydanRealTimeData &data, uint wiringPhases)
{
    if (!phaseMeasurementsAvailable(data) || qAbs(*data.voltageMeasure[0]) < PhasePresentV)
        return WiringCheck::NotChecked;

    bool present = false;
    bool absent = false;
    bool unclear = false;
    for (int i = 1; i < 3; ++i) {
        const double v = qAbs(*data.voltageMeasure[i]);
        if (v >= PhasePresentV)
            present = true;
        else if (v <= PhaseAbsentV)
            absent = true;
        else
            unclear = true;
    }
    if (wiringPhases < 3) {
        if (present)
            return WiringCheck::VoltageOnL2L3;
    } else if (absent) {
        // Only a charger set to draw on three phases closes L2/L3: in single-phase (or
        // mixed) mode it leaves them open itself, and their absence says nothing of the
        // wiring.
        const ChargeMode mode = chargeMode(data);
        const bool threePhaseMode = mode == ChargeMode::Three || !data.chargeMode;
        return threePhaseMode ? WiringCheck::NoVoltageOnL2L3 : WiringCheck::NotChecked;
    }
    return unclear ? WiringCheck::NotChecked : WiringCheck::Consistent;
}

uint phaseCount(const TrydanRealTimeData &data, uint wiringPhases)
{
    const uint wiring = wiringPhases >= 3 ? 3 : 1;

    if (charging(data) && phaseMeasurementsAvailable(data)) {
        uint active = 0;
        for (double a : phaseCurrents(data)) {
            if (a != 0.0)
                ++active;
        }
        if (active > 0)
            return active;
    }

    if (chargeMode(data) == ChargeMode::Single)
        return 1;
    return wiring;
}

DesiredPhases desiredPhases(const TrydanRealTimeData &data, uint wiringPhases)
{
    DesiredPhases result;
    if (wiringPhases < 3) {
        result.possibleValues = {1u};
        result.value = 1u;
        return result;
    }

    switch (chargeMode(data)) {
    case ChargeMode::Single:
        result.value = 1u;
        break;
    case ChargeMode::Three:
        result.value = 3u;
        break;
    case ChargeMode::Mixed:
        // The charger picks the phases itself: there is no fixed 1 or 3 to publish.
        // The measured phaseCount says what it is doing.
        break;
    case ChargeMode::Unknown:
        if (!data.chargeMode) {
            // No ChargeMode at all: this firmware cannot switch. Three phases wired,
            // three phases used.
            result.possibleValues = {3u};
            result.value = 3u;
            return result;
        }
        // A ChargeMode value outside 0..2: switching is there, the value is unknown.
        break;
    }
    result.possibleValues = {1u, 3u};
    result.canSwitch = true;
    return result;
}

int chargeModeFor(uint phases)
{
    return phases == 1 ? 0 : 1;
}

CurrentRange currentRange(const TrydanRealTimeData &data)
{
    CurrentRange range;
    range.min = qMax(MinimumCurrentA, data.minIntensity.value_or(MinimumCurrentA));
    range.max = qMax(range.min, data.maxIntensity.value_or(range.min));
    return range;
}

CurrentCommand currentCommand(const TrydanRealTimeData &data, double requested)
{
    const CurrentRange range = currentRange(data);
    CurrentCommand command;
    // Written so that a NaN stops charging too.
    if (!(requested >= range.min)) {
        if (!data.paused || *data.paused != 1)
            command.kind = CurrentCommand::Pause;
        return command;
    }
    const int ampere = qMin(qFloor(requested), range.max);
    if (!data.intensity || *data.intensity != ampere) {
        command.kind = CurrentCommand::SetIntensity;
        command.ampere = ampere;
    }
    return command;
}

}
