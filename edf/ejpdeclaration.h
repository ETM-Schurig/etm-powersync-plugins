// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef EJPDECLARATION_H
#define EJPDECLARATION_H

#include "contract.h"
#include "ejpcalendar.h"

#include <QDate>
#include <QDateTime>
#include <QString>

// The two actions of the contract thing, "Déclarer demain jour EJP" and "Annuler la
// déclaration de demain", as plain functions of the contract, the calendar and the box local
// time, with the message given to the user.
//
// Rules (EjpCalendar for the details):
//  - EJP contract only; any other contract is refused.
//  - The target is the next EJP day whose peak has not started: the day of the next 07:00.
//    From 00:00 to 07:00 that is the calendar day in progress.
//  - A target outside 1 November .. 31 March is refused.
//  - Declaring twice, or cancelling when nothing is declared, changes nothing (accepted).
//  - Cancelling is refused once the peak of the declared day has started (07:00): the TIC of
//    the meter decides.
namespace EjpDeclaration {

struct Outcome {
    bool accepted = false;   // false: the action fails with message
    bool changed = false;    // the calendar changed (to be saved and published)
    QDate day;               // the EJP day concerned
    QString message;         // for the user, in clear
};

Outcome declareTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local);
Outcome cancelTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local);

}

#endif // EJPDECLARATION_H
