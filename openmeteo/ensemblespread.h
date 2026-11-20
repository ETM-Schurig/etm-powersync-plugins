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

#ifndef ENSEMBLESPREAD_H
#define ENSEMBLESPREAD_H

#include <QVariantList>
#include <QVariantMap>

// Weather spread of a plane, from one ensemble reply (CONTRACT_A sec. 2.5), and its
// freshness. Kept free of nymea and of the clock, and tested from a recorded reply.
namespace EnsembleSpread {

static const int IntervalS = 10800;
// Beyond twice the interval, the spread is no longer published as current (sec. 1.5).
static const int MaxAgeS = 2 * IntervalS;

// [today, tomorrow] in percent, rounded to 0.1: coefficient of variation of the daily
// GTI energy across the members. null for a day that cannot be computed (fewer than 5
// complete members, or a zero mean). Empty when the reply carries no time series or no
// member - a failure. Both null is a failure too: the caller checks.
QVariantList daily(const QVariantMap &reply);

// True while the last successful spread is at most MaxAgeS old. spreadTime 0 = never.
bool fresh(qint64 spreadTime, qint64 now);

// The value an invalid spread state carries: NaN. A nymea state cannot hold null - an
// invalid QVariant is refused by Thing::setStateValue (type mismatch, change discarded) -
// but a NaN double is accepted and Qt's JSON writer, through which nymea sends every
// state, writes it as null. Never 0: 0 % would read as "no uncertainty".
double invalid();

}

#endif // ENSEMBLESPREAD_H
