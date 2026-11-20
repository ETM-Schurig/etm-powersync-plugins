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

#include "sitecoordinates.h"

#include <QtGlobal>

bool SiteCoordinates::read(const QVariant &latitudeValue, const QVariant &longitudeValue,
                           double &latitude, double &longitude)
{
    double lat, lon;
    if (!readOne(latitudeValue, 90.0, lat) || !readOne(longitudeValue, 180.0, lon))
        return false;
    latitude = lat;
    longitude = lon;
    return true;
}

bool SiteCoordinates::readOne(const QVariant &value, double bound, double &result)
{
    // Absent: no param at all, or the null left by a failed conversion of a missing key.
    if (!value.isValid() || value.isNull())
        return false;
    bool ok = false;
    const double v = value.toDouble(&ok);
    // Out of range covers the Unset sentinel. NaN fails both comparisons, hence qIsFinite.
    if (!ok || !qIsFinite(v) || v < -bound || v > bound)
        return false;
    result = v;
    return true;
}
