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

#ifndef SITECOORDINATES_H
#define SITECOORDINATES_H

#include <QVariant>

// Whether a site has coordinates to forecast for (CONTRACT_A sec. 4).
//
// A site without entered coordinates is not set up: no request leaves, and setup says
// why. The declared default is a sentinel outside the valid range (Unset), so a site
// added without touching the fields is refused rather than forecast for a placeholder
// point. 0 / 0 is a real point and is accepted.
//
// Kept free of nymea, like RetrySchedule, so the rule is tested on its own.
class SiteCoordinates
{
public:
    // The defaultValue of both params in integrationpluginopenmeteo.json.
    static constexpr double Unset = 999.0;

    // True when both values are present, numeric, finite and within [-90, 90] for the
    // latitude and [-180, 180] for the longitude, bounds included. On success the two
    // doubles are written to latitude and longitude; on failure they are left untouched.
    static bool read(const QVariant &latitudeValue, const QVariant &longitudeValue,
                     double &latitude, double &longitude);

private:
    static bool readOne(const QVariant &value, double bound, double &result);
};

#endif // SITECOORDINATES_H
