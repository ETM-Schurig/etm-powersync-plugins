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

#ifndef RETRYSCHEDULE_H
#define RETRYSCHEDULE_H

#include <QHash>
#include <QString>
#include <functional>

// When to ask an upstream source again (CONTRACT_A sec. 3.1).
//
// One entry per (thing, source). After a success the next attempt is one source interval
// later; after a failure it is 5 min x 2^(n-1), n the number of consecutive failures,
// capped at the interval. Both carry +/-60 s of jitter, rolled once when the attempt is
// scheduled, so that a fleet does not hit the API in lockstep.
//
// Kept free of nymea and of the wall clock: every call takes "now" in Unix seconds, and
// the jitter is injected, so the whole schedule is tested on a simulated clock.
class RetrySchedule
{
public:
    static const int FirstRetryS = 300;
    static const int JitterS = 60;

    // Returns seconds in [-JitterS, JitterS]. Empty = uniform random.
    using Jitter = std::function<int()>;
    explicit RetrySchedule(Jitter jitter = Jitter());

    // 5 min x 2^(failures-1), capped at intervalS; intervalS itself when failures <= 0.
    static int retryDelayS(int failures, int intervalS);

    // True when the source has never been asked, or when its next attempt is reached.
    bool due(const QString &key, qint64 now) const;

    // An attempt leaves. It counts as a failure until its reply says otherwise: a reply
    // that never arrives must not leave the source blocked, nor asked every minute.
    void started(const QString &key, int intervalS, qint64 now);
    // The reply carried usable data: the count goes back to zero.
    void succeeded(const QString &key, int intervalS, qint64 now);
    // Network or HTTP error, unreadable body, API error, or a 200 that carries nothing.
    void failed(const QString &key, int intervalS, qint64 now);

    void remove(const QString &key);

    // Consecutive attempts without success, the one in flight included.
    int failures(const QString &key) const;
    // 0 = never.
    qint64 lastSuccess(const QString &key) const;
    qint64 nextAttempt(const QString &key) const;

private:
    struct Entry {
        int failures = 0;
        qint64 lastSuccess = 0;
        qint64 nextAttempt = 0;
    };
    QHash<QString, Entry> m_entries;
    Jitter m_jitter;

    int jitter() const;
};

#endif // RETRYSCHEDULE_H
