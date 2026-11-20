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

#include "retryschedule.h"

#include <QRandomGenerator>
#include <QtGlobal>

RetrySchedule::RetrySchedule(Jitter jitter) :
    m_jitter(jitter)
{
}

int RetrySchedule::retryDelayS(int failures, int intervalS)
{
    if (failures <= 0)
        return intervalS;
    // 300 << 20 already exceeds any interval; stop doubling before it overflows.
    if (failures > 20)
        return intervalS;
    return qMin(FirstRetryS << (failures - 1), intervalS);
}

bool RetrySchedule::due(const QString &key, qint64 now) const
{
    if (!m_entries.contains(key))
        return true;
    return now >= m_entries.value(key).nextAttempt;
}

void RetrySchedule::started(const QString &key, int intervalS, qint64 now)
{
    Entry &entry = m_entries[key];
    entry.failures++;
    entry.nextAttempt = now + retryDelayS(entry.failures, intervalS) + jitter();
}

void RetrySchedule::succeeded(const QString &key, int intervalS, qint64 now)
{
    Entry &entry = m_entries[key];
    entry.failures = 0;
    entry.lastSuccess = now;
    entry.nextAttempt = now + intervalS + jitter();
}

void RetrySchedule::failed(const QString &key, int intervalS, qint64 now)
{
    Entry &entry = m_entries[key];
    // started() already counted this attempt; a failure reported without it counts here.
    if (entry.failures == 0)
        entry.failures = 1;
    // The delay runs from the failure, not from the attempt: a reply that took two
    // minutes to fail must not be retried at once.
    entry.nextAttempt = now + retryDelayS(entry.failures, intervalS) + jitter();
}

void RetrySchedule::remove(const QString &key)
{
    m_entries.remove(key);
}

int RetrySchedule::failures(const QString &key) const
{
    return m_entries.value(key).failures;
}

qint64 RetrySchedule::lastSuccess(const QString &key) const
{
    return m_entries.value(key).lastSuccess;
}

qint64 RetrySchedule::nextAttempt(const QString &key) const
{
    return m_entries.value(key).nextAttempt;
}

int RetrySchedule::jitter() const
{
    const int j = m_jitter ? m_jitter() : QRandomGenerator::global()->bounded(-JitterS, JitterS + 1);
    return qBound(-JitterS, j, JitterS);
}
