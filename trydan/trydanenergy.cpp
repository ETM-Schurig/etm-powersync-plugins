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

#include "trydanenergy.h"

void TrydanEnergy::restore(bool sessionOpen, double lastRawKWh, double sessionKWh, double totalKWh)
{
    m_sessionOpen = sessionOpen;
    m_absences = 0;
    m_sessionReference = lastRawKWh;
    m_totalReference = lastRawKWh;
    m_sessionEnergy = sessionKWh;
    m_totalEnergy = totalKWh;
}

double TrydanEnergy::increment(double raw, double reference)
{
    const double difference = raw - reference;
    if (difference >= 0.0)
        return difference;
    // A tiny fall is read noise, not a reset. The caller still moves its reference to the
    // reading, so the rise that follows is not counted twice; the dip itself is counted
    // again on the way up, at most NoiseKWh per dip.
    if (difference > -NoiseKWh)
        return 0.0;
    // The charger reset its counter: what is left to add is what it counted since.
    return raw;
}

TrydanEnergy::Result TrydanEnergy::update(double rawKWh, bool pluggedIn)
{
    Result result;
    result.chargerReset = rawKWh <= m_sessionReference - NoiseKWh
                          || rawKWh <= m_totalReference - NoiseKWh;

    // Lifetime total: whatever the charger delivered, session or not (a charge started by
    // hand on the charger counts too). Never reset.
    m_totalEnergy += increment(rawKWh, m_totalReference);
    m_totalReference = rawKWh;

    // Session: unplug confirmed on AbsencesToClose successful polls.
    m_absences = pluggedIn ? 0 : m_absences + 1;
    if (m_sessionOpen && m_absences >= AbsencesToClose) {
        // The value stays readable until the next plug-in, which alone opens a new session:
        // "how much did the night deliver" is asked in the morning, car gone.
        m_sessionOpen = false;
        result.sessionClosed = true;
    }

    if (!m_sessionOpen && pluggedIn) {
        // Start from the raw value, not from zero: plugged in during an outage of the
        // link, the charger has already counted energy that belongs to this session.
        m_sessionOpen = true;
        m_sessionEnergy = rawKWh;
        result.sessionOpened = true;
    } else if (m_sessionOpen) {
        m_sessionEnergy += increment(rawKWh, m_sessionReference);
    }
    m_sessionReference = rawKWh;

    result.sessionEnergy = m_sessionEnergy;
    result.totalEnergy = m_totalEnergy;
    return result;
}
