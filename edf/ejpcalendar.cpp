// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "ejpcalendar.h"

#include <QStringList>
#include <QTimeZone>
#include <algorithm>

namespace {

QDate seasonEndOf(const QDate &start)
{
    return QDate(start.year() + 1, 3, 31);
}

QVariantMap statusMap(const QMap<QDate, EjpCalendar::Status> &map)
{
    QVariantMap result;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        result.insert(it.key().toString(Qt::ISODate), EjpCalendar::statusCode(it.value()));
    return result;
}

QMap<QDate, EjpCalendar::Status> statusesFrom(const QVariantMap &map)
{
    QMap<QDate, EjpCalendar::Status> result;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        const QDate date = QDate::fromString(it.key(), Qt::ISODate);
        const QString value = it.value().toString();
        if (!date.isValid())
            continue;
        if (value == QStringLiteral("pointe"))
            result.insert(date, EjpCalendar::Status::Peak);
        else if (value == QStringLiteral("normal"))
            result.insert(date, EjpCalendar::Status::Normal);
    }
    return result;
}

}

bool EjpCalendar::inSeason(const QDate &date)
{
    return date.month() >= 11 || date.month() <= 3;
}

QDate EjpCalendar::seasonStart(const QDate &date)
{
    // The current season, or outside the season the last one.
    return QDate(date.month() >= 11 ? date.year() : date.year() - 1, 11, 1);
}

QDate EjpCalendar::ejpDayOf(const QDateTime &local)
{
    return local.time() < QTime(7, 0) ? local.date().addDays(-1) : local.date();
}

QDateTime EjpCalendar::peakStart(const QDate &day, const QTimeZone &zone)
{
    return QDateTime(day, QTime(7, 0), zone);
}

QDateTime EjpCalendar::peakEnd(const QDate &day, const QTimeZone &zone)
{
    return QDateTime(day.addDays(1), QTime(1, 0), zone);
}

bool EjpCalendar::observeTic(const QDateTime &local, TicReading::Period period, bool ejpNotice)
{
    const QTime time = local.time();
    QDate day;
    Status status = Status::Unknown;

    if (period == TicReading::Period::EjpMobilePeak) {
        // 07:00 .. 01:00, a few minutes of clock drift between the meter and the box allowed.
        if (time >= QTime(6, 55) || time < QTime(1, 10)) {
            day = time < QTime(6, 55) ? local.date().addDays(-1) : local.date();
            status = Status::Peak;
        }
    } else if (ejpNotice) {
        // PEJP: the notice 30 minutes before the peak of today.
        if (time >= QTime(6, 0) && time < QTime(7, 30)) {
            day = local.date();
            status = Status::Peak;
        }
    } else if (period == TicReading::Period::EjpNormal) {
        if (time >= QTime(7, 5)) {
            day = local.date();
            status = Status::Normal;
        } else if (time < QTime(0, 55)) {
            day = local.date().addDays(-1);
            status = Status::Normal;
        }
    }

    if (status == Status::Unknown || !day.isValid() || !inSeason(day))
        return false;
    const Status previous = m_tic.value(day, Status::Unknown);
    // A peak seen on the meter stays a peak: normal hours seen later the same EJP day (a
    // glitch, a meter clock off by hours) do not undo it.
    if (previous == Status::Peak || previous == status)
        return false;
    m_tic.insert(day, status);
    return true;
}

QDate EjpCalendar::declarationTarget(const QDateTime &local)
{
    return ejpDayOf(local).addDays(1);
}

EjpCalendar::DeclareResult EjpCalendar::declareNextDay(const QDateTime &local, QDate *target)
{
    const QDate day = declarationTarget(local);
    if (target)
        *target = day;
    if (!inSeason(day))
        return DeclareResult::OutOfSeason;
    if (m_declared.contains(day))
        return DeclareResult::AlreadyDeclared;
    m_declared.insert(day);
    return DeclareResult::Declared;
}

EjpCalendar::CancelResult EjpCalendar::cancelNextDay(const QDateTime &local, QDate *target)
{
    const QDate next = declarationTarget(local);
    if (m_declared.remove(next)) {
        if (target)
            *target = next;
        return CancelResult::Cancelled;
    }
    // The EJP day in progress started at 07:00: its declaration stays, the TIC decides.
    const QDate current = ejpDayOf(local);
    if (target)
        *target = m_declared.contains(current) ? current : next;
    return m_declared.contains(current) ? CancelResult::AlreadyStarted : CancelResult::NothingToCancel;
}

EjpCalendar::Day EjpCalendar::day(const QDate &date) const
{
    if (!inSeason(date))
        return Day { Status::Normal, Source::Season };
    const Status tic = m_tic.value(date, Status::Unknown);
    if (tic != Status::Unknown)
        return Day { tic, Source::Tic };
    if (m_declared.contains(date))
        return Day { Status::Peak, Source::Manual };
    return Day {};
}

EjpCalendar::Day EjpCalendar::dayOrPresumed(const QDate &date) const
{
    const Day known = day(date);
    if (known.status != Status::Unknown)
        return known;
    return Day { Status::Normal, Source::Presumed };
}

QString EjpCalendar::discrepancy(const QDate &date) const
{
    if (!m_declared.contains(date) || m_tic.value(date, Status::Unknown) != Status::Normal)
        return QString();
    return QStringLiteral("%1 : déclaré jour EJP, la TIC lit des heures normales (déclaration contredite, jour normal)")
            .arg(date.toString(Qt::ISODate));
}

void EjpCalendar::setCountStart(const QDate &countFrom, int priorPeakDays)
{
    m_countFrom = countFrom;
    m_priorPeakDays = qBound(0, priorPeakDays, SeasonPeakDays);
}

QDate EjpCalendar::seasonCountStart(const QDate &today, int *priorPeakDays) const
{
    const QDate start = seasonStart(today);
    // The season of the installation is the one of the last day before countFrom.
    const bool applies = m_countFrom.isValid() && seasonStart(m_countFrom.addDays(-1)) == start;
    if (priorPeakDays)
        *priorPeakDays = applies ? m_priorPeakDays : 0;
    return applies ? qMax(start, m_countFrom) : start;
}

int EjpCalendar::seasonPeakDays(const QDate &today) const
{
    int count = 0;
    const QDate from = seasonCountStart(today, &count);
    const QDate end = qMin(today, seasonEndOf(seasonStart(today)));
    for (QDate date = from; date <= end; date = date.addDays(1)) {
        if (day(date).status == Status::Peak)
            count++;
    }
    return count;
}

int EjpCalendar::seasonUnknownDays(const QDate &today) const
{
    const QDate from = seasonCountStart(today);
    const QDate end = qMin(today.addDays(-1), seasonEndOf(seasonStart(today)));
    int count = 0;
    for (QDate date = from; date <= end; date = date.addDays(1)) {
        if (day(date).status == Status::Unknown)
            count++;
    }
    return count;
}

bool EjpCalendar::seasonCountComplete(const QDate &today) const
{
    return seasonUnknownDays(today) == 0;
}

QVariantMap EjpCalendar::toVariant() const
{
    QVariantMap map;
    map.insert(QStringLiteral("version"), 2);
    map.insert(QStringLiteral("tic"), statusMap(m_tic));
    QList<QDate> declared(m_declared.begin(), m_declared.end());
    std::sort(declared.begin(), declared.end());
    QStringList manual;
    for (const QDate &date : declared)
        manual << date.toString(Qt::ISODate);
    map.insert(QStringLiteral("manuel"), manual);
    return map;
}

EjpCalendar EjpCalendar::fromVariant(const QVariantMap &map)
{
    EjpCalendar calendar;
    // Version 1 (before the declarations) holds the TIC days only.
    const int version = map.value(QStringLiteral("version")).toInt();
    if (version != 1 && version != 2)
        return calendar;
    calendar.m_tic = statusesFrom(map.value(QStringLiteral("tic")).toMap());
    for (const QString &text : map.value(QStringLiteral("manuel")).toStringList()) {
        const QDate date = QDate::fromString(text, Qt::ISODate);
        if (date.isValid() && inSeason(date))
            calendar.m_declared.insert(date);
    }
    return calendar;
}

void EjpCalendar::prune(const QDate &today)
{
    const QDate start = seasonStart(today);
    for (auto it = m_tic.begin(); it != m_tic.end();) {
        if (it.key() < start)
            it = m_tic.erase(it);
        else
            ++it;
    }
    for (auto it = m_declared.begin(); it != m_declared.end();) {
        if (*it < start)
            it = m_declared.erase(it);
        else
            ++it;
    }
}

QString EjpCalendar::statusCode(Status status)
{
    switch (status) {
    case Status::Normal: return QStringLiteral("normal");
    case Status::Peak: return QStringLiteral("pointe");
    case Status::Unknown: break;
    }
    return QStringLiteral("inconnu");
}

QString EjpCalendar::sourceCode(Source source)
{
    switch (source) {
    case Source::Season: return QStringLiteral("saison");
    case Source::Tic: return QStringLiteral("tic");
    case Source::Manual: return QStringLiteral("manuel");
    case Source::Presumed: return QStringLiteral("presume");
    case Source::None: break;
    }
    return QStringLiteral("aucune");
}
