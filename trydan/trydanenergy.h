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

#ifndef TRYDANENERGY_H
#define TRYDANENERGY_H

// sessionEnergy and totalEnergyConsumed, built from ChargeEnergy.
//
// ChargeEnergy is exact while a charge runs, but the charger resets it to 0 whenever a
// charge stops - cable still plugged in, and so also on every pause the energy manager
// commands around a phase switch. It survives a power cut of the charger. It has no
// lifetime total. Published as such, the session would read "nothing delivered" after
// any pause, so both values are accumulated here from its increments.
//
// One increment rule for both (increment()): rising -> the difference; falling by less
// than NoiseKWh -> nothing; falling more -> the charger reset, add what it counted since.
// Each value keeps its own reference, so the order of the updates does not matter.
//
// update() is to be called on SUCCESSFUL polls only. That is what makes a false unplug
// impossible to fabricate with a dead link: a failed poll freezes the unplug count, it
// does not advance it. A session is closed after AbsencesToClose successful polls in a
// row without a vehicle.
//
// Accepted limit: the energy delivered between the last successful poll and a reset of
// the charger is lost, bounded by one poll interval.
class TrydanEnergy
{
public:
    static constexpr double NoiseKWh = 0.001;
    static constexpr int AbsencesToClose = 3;

    struct Result {
        double sessionEnergy = 0;  // kWh
        double totalEnergy = 0;    // kWh
        bool sessionOpened = false;
        bool sessionClosed = false;
        bool chargerReset = false; // ChargeEnergy fell: the charger reset its counter
    };

    // State recovered at setup, before the first poll: whether a session was open, the
    // last raw ChargeEnergy published and the two published values (all from the state
    // cache and the plugin storage).
    void restore(bool sessionOpen, double lastRawKWh, double sessionKWh, double totalKWh);

    Result update(double rawKWh, bool pluggedIn);

    bool sessionOpen() const { return m_sessionOpen; }
    double sessionEnergy() const { return m_sessionEnergy; }
    double totalEnergy() const { return m_totalEnergy; }

    static double increment(double raw, double reference);

private:
    bool m_sessionOpen = false;
    int m_absences = 0;
    double m_sessionReference = 0;
    double m_totalReference = 0;
    double m_sessionEnergy = 0;
    double m_totalEnergy = 0;
};

#endif // TRYDANENERGY_H
