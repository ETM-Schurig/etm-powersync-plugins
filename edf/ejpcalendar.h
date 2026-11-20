// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef EJPCALENDAR_H
#define EJPCALENDAR_H

#include "ticreading.h"

#include <QDate>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QVariantMap>

// The EJP days: what is known of each one, and from where.
//
// An "EJP day" D is the day whose mobile peak, if any, runs from D 07:00 to D+1 01:00 (box
// local time). The season runs from 1 November to 31 March, 22 peak days at most. Outside it
// every day is normal, known from the calendar alone.
//
// Sources of a day in season, by precedence:
//  1. the TIC of the meter (PTEC = PM.. or PEJP for a peak, PTEC = HN.. after 07:05 for a
//     normal day): it is what the meter bills, it always wins;
//  2. a manual declaration "tomorrow is an EJP day", made by the user after the alert of EDF
//     (SMS, e-mail): a peak, until the TIC says otherwise;
//  3. nothing: unknown. For prices a day still unknown is presumed normal (dayOrPresumed()):
//     the TIC gives no notice the day before, so the declaration is the only way to know a
//     peak in advance, and about 130 of the 151 days of a season are normal. A peak neither
//     declared nor read becomes known at 06:30 (PEJP) or 07:00 (PTEC) on a linked meter.
// A declared day that the TIC sees in normal hours is a normal day; the contradiction is
// published (discrepancy()). The plugin opens no network connection.
class EjpCalendar
{
public:
    enum class Status { Unknown, Normal, Peak };
    enum class Source { None, Season, Tic, Manual, Presumed };

    struct Day {
        Status status = Status::Unknown;
        Source source = Source::None;
    };

    static bool inSeason(const QDate &date);           // 1 Nov .. 31 Mar
    static QDate seasonStart(const QDate &date);        // 1 Nov of the season of date (in season only)
    static constexpr int SeasonPeakDays = 22;

    // The EJP day whose peak window [07:00, 01:00) contains this local time, or whose normal
    // hours 07:00-24:00 contain it. 00:00-01:00 belongs to the day before.
    static QDate ejpDayOf(const QDateTime &local);
    // Peak window of day D, box local time.
    static QDateTime peakStart(const QDate &day, const QTimeZone &zone);
    static QDateTime peakEnd(const QDate &day, const QTimeZone &zone);

    // What the TIC says at this local time. Returns true when it changed something.
    bool observeTic(const QDateTime &local, TicReading::Period period, bool ejpNotice);

    // Manual declaration. It targets the next EJP day whose peak has not started: the day of
    // the next 07:00 (from 00:00 to 07:00 that is the calendar day in progress, not the day
    // after: its peak starts at 07:00 the same morning).
    enum class DeclareResult { Declared, AlreadyDeclared, OutOfSeason };
    enum class CancelResult { Cancelled, NothingToCancel, AlreadyStarted };
    static QDate declarationTarget(const QDateTime &local);
    DeclareResult declareNextDay(const QDateTime &local, QDate *target = nullptr);
    // Cancels the declaration of the next day. Refused once the peak of a declared day has
    // started (from 07:00 of that day): then the TIC decides.
    CancelResult cancelNextDay(const QDateTime &local, QDate *target = nullptr);
    bool isDeclared(const QDate &date) const { return m_declared.contains(date); }

    Day day(const QDate &date) const;
    // day(), with an unknown day presumed normal (source Presumed). For prices and the
    // published status only: the season counter keeps the day unknown.
    Day dayOrPresumed(const QDate &date) const;
    Status ticStatus(const QDate &date) const { return m_tic.value(date, Status::Unknown); }
    // "" or, when the TIC contradicts a declaration (normal hours on a declared day), both.
    QString discrepancy(const QDate &date) const;

    // Season counter, as of today = the peak days entered at installation + the peak days
    // known to the box (TIC or declaration) from the start of its count.
    //
    // At installation in the course of a season the user enters the peak days already passed,
    // the one in progress included. The box counts from countFrom = the day of the next 07:00
    // after the installation (declarationTarget()): every EJP day whose peak had started is
    // in the entered number, every later day is counted by the box, and a day before countFrom
    // seen on the meter (00:30 on the day of the installation: the peak of the day before) is
    // not counted twice. The entered number applies to the season of the installation only.
    void setCountStart(const QDate &countFrom, int priorPeakDays);
    QDate countFrom() const { return m_countFrom; }
    // The first day the box counts in the season of today, and the entered number that
    // applies to that season (0 when the installation was in another season).
    QDate seasonCountStart(const QDate &today, int *priorPeakDays = nullptr) const;

    // Peak days of the season, and whether every day the box counts, up to yesterday, has a
    // known status (otherwise the count is a lower bound: the box was off, no TIC, nothing
    // declared).
    int seasonPeakDays(const QDate &today) const;
    bool seasonCountComplete(const QDate &today) const;
    int seasonUnknownDays(const QDate &today) const;

    // Persistence (plugin storage): the statuses of the current season.
    QVariantMap toVariant() const;
    static EjpCalendar fromVariant(const QVariantMap &map);
    // Forget the days of previous seasons.
    void prune(const QDate &today);

    static QString statusCode(Status status);   // "normal", "pointe", "inconnu"
    static QString sourceCode(Source source);   // "tic", "manuel", "saison", "presume", "aucune"

private:
    QMap<QDate, Status> m_tic;
    QSet<QDate> m_declared;
    QDate m_countFrom;
    int m_priorPeakDays = 0;
};

#endif // EJPCALENDAR_H
