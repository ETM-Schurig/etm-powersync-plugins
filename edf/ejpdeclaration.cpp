// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "ejpdeclaration.h"

namespace {

QString frenchDate(const QDate &date)
{
    return date.toString(QStringLiteral("dd/MM/yyyy"));
}

EjpDeclaration::Outcome notEjp()
{
    EjpDeclaration::Outcome outcome;
    outcome.message = QStringLiteral("La déclaration d'un jour EJP ne concerne que le contrat EJP : ce contrat n'est pas un contrat EJP.");
    return outcome;
}

}

namespace EjpDeclaration {

Outcome declareTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local)
{
    if (type != Contract::Type::Ejp)
        return notEjp();
    Outcome outcome;
    switch (calendar.declareNextDay(local, &outcome.day)) {
    case EjpCalendar::DeclareResult::OutOfSeason:
        outcome.message = QStringLiteral("Le %1 est hors de la saison EJP (1er novembre - 31 mars) : ce ne peut pas être un jour de pointe.")
                .arg(frenchDate(outcome.day));
        break;
    case EjpCalendar::DeclareResult::AlreadyDeclared:
        outcome.accepted = true;
        outcome.message = QStringLiteral("Le %1 est déjà déclaré jour EJP.").arg(frenchDate(outcome.day));
        break;
    case EjpCalendar::DeclareResult::Declared:
        outcome.accepted = true;
        outcome.changed = true;
        outcome.message = QStringLiteral("Jour EJP déclaré : pointe du %1 07:00 au %2 01:00, jusqu'à ce que le compteur dise autre chose.")
                .arg(frenchDate(outcome.day), frenchDate(outcome.day.addDays(1)));
        break;
    }
    return outcome;
}

Outcome cancelTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local)
{
    if (type != Contract::Type::Ejp)
        return notEjp();
    Outcome outcome;
    switch (calendar.cancelNextDay(local, &outcome.day)) {
    case EjpCalendar::CancelResult::AlreadyStarted:
        outcome.message = QStringLiteral("La pointe du %1 a commencé à 07:00 : la déclaration ne peut plus être annulée, le compteur (TIC) fait foi.")
                .arg(frenchDate(outcome.day));
        break;
    case EjpCalendar::CancelResult::NothingToCancel:
        outcome.accepted = true;
        outcome.message = QStringLiteral("Aucune déclaration à annuler pour le %1.").arg(frenchDate(outcome.day));
        break;
    case EjpCalendar::CancelResult::Cancelled:
        outcome.accepted = true;
        outcome.changed = true;
        outcome.message = QStringLiteral("Déclaration du %1 annulée.").arg(frenchDate(outcome.day));
        break;
    }
    return outcome;
}

}
