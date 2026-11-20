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

#ifndef TRYDANCLIENT_H
#define TRYDANCLIENT_H

#include "trydanrealtimedata.h"

#include <QElapsedTimer>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

// The outcome of one request made through TrydanClient. Finishes exactly once, always
// from the event loop (never inside the call that created it), and is deleted by the
// client afterwards: read it in a slot connected to finished().
class TrydanReply : public QObject
{
    Q_OBJECT
public:
    enum Error {
        NoError,
        NetworkError, // no answer: connection refused, timeout, ...
        Rejected,     // the charger answered a /write/ with something else than "OK"
        NotTaken,     // the charger said "OK" but the value read back differs
        Unsupported,  // the charger does not report the parameter written
        Aborted       // the client was destroyed first
    };
    Q_ENUM(Error)

    Error error() const { return m_error; }
    QString errorString() const { return m_errorString; }
    // What was sent, as "<Param>=<value>"; empty when nothing had to be written.
    QString written() const { return m_written; }
    bool isFinished() const { return m_finished; }

    // Nobody waits for this outcome any more (the action timed out in nymea): a write not
    // sent yet is dropped rather than sent late. One already sent completes.
    void abandon() { m_abandoned = true; }
    bool isAbandoned() const { return m_abandoned; }

signals:
    void finished();

private:
    friend class TrydanClient;
    explicit TrydanReply(QObject *parent) : QObject(parent) {}
    void finish(Error error, const QString &errorString);

    Error m_error = NoError;
    QString m_errorString;
    QString m_written;
    bool m_finished = false;
    bool m_abandoned = false;
};

// HTTP client of one V2C Trydan: GET /RealTimeData and GET /write/<Param>=<int>.
//
// One request at a time, in arrival order. The charger is an ESP32 on Wi-Fi whose stack
// degrades under concurrent or rapid requests, and the energy manager issues several
// actions in the same instant: they are queued here, never sent in parallel. A poll tick
// that finds anything queued or running is skipped, so polls never pile up.
//
// A write is one gesture: send it, require "OK", then read RealTimeData back and require
// the written value. Up to verifyAttempts reads, verifyDelayMs apart, because the charger
// may report the new value one reading late; the queue stays held meanwhile.
//
// Bounds: with the defaults a write takes at most 4 + 4 + 1 + 4 = 13 s on a link that
// answers late, under the 15 s nymea gives an action.
//
// reachable() turns false after errorLimit failed exchanges in a row (no answer at all;
// a refused write is an answer) and true again on the next successful read.
//
// Free of nymea, so that it runs against a local fake server in the tests.
class TrydanClient : public QObject
{
    Q_OBJECT
public:
    static constexpr int DefaultRequestTimeoutMs = 4000;
    static constexpr int DefaultPollIntervalMs = 30000;
    static constexpr int DefaultErrorLimit = 3;
    static constexpr int DefaultVerifyAttempts = 2;
    static constexpr int DefaultVerifyDelayMs = 1000;

    explicit TrydanClient(const QString &host, quint16 port = 80, QObject *parent = nullptr);
    ~TrydanClient() override;

    QString host() const { return m_host; }
    void setHost(const QString &host);

    void setRequestTimeout(int ms) { m_requestTimeoutMs = ms; }
    void setPollInterval(int ms);
    void setErrorLimit(int limit) { m_errorLimit = qMax(1, limit); }
    void setVerify(int attempts, int delayMs);

    void startPolling();
    void stopPolling();

    bool reachable() const { return m_reachable; }
    int consecutiveFailures() const { return m_failures; }
    bool hasData() const { return m_hasData; }
    const TrydanRealTimeData &data() const { return m_data; }
    // Milliseconds since the last successful read, -1 without one.
    qint64 dataAgeMs() const;
    // Requests queued or running.
    int pending() const;

    // A fresh read. Joins a read already queued or running rather than adding one.
    TrydanReply *refresh();
    // Write one parameter and check that the charger took it. Not sent when the last
    // reading, at the moment the write leaves the queue, already shows the value.
    TrydanReply *write(const QString &param, int value);
    // The maxChargingCurrent gesture: TrydanModel::currentCommand(), decided on the last
    // reading at the moment it leaves the queue - not when it is queued, so that a gesture
    // queued behind another one (the energy manager sends several at once) is decided on
    // what that one left. Then a write like write().
    TrydanReply *setChargingCurrent(double requested);

signals:
    // After every successful read, including the ones that verify a write.
    void dataReceived(const TrydanRealTimeData &data);
    void reachableChanged(bool reachable);
    // A failed exchange, for the log.
    void exchangeFailed(const QString &what, const QString &error);

private:
    enum class JobType { Read, Write, ChargingCurrent };
    struct Job {
        JobType type = JobType::Read;
        QString param;
        int value = 0;
        double requested = 0; // ChargingCurrent
        int attemptsLeft = 0;
        bool verifying = false;
        QList<QPointer<TrydanReply>> replies;
    };

    void enqueue(const Job &job);
    void startNext();
    void sendRead();
    void sendWrite();
    void onReadFinished(QNetworkReply *reply);
    void onWriteFinished(QNetworkReply *reply);
    void finishCurrent(TrydanReply::Error error, const QString &errorString);
    void recordFailure(const QString &what, const QString &error);
    void recordAnswer();
    void setReachable(bool reachable);
    QNetworkReply *get(const QString &path);

    QNetworkAccessManager m_nam;
    QString m_host;
    quint16 m_port = 80;
    int m_requestTimeoutMs = DefaultRequestTimeoutMs;
    int m_errorLimit = DefaultErrorLimit;
    int m_verifyAttempts = DefaultVerifyAttempts;
    int m_verifyDelayMs = DefaultVerifyDelayMs;

    QTimer m_pollTimer;
    QList<Job> m_queue;
    bool m_busy = false;
    Job m_current;
    QPointer<QNetworkReply> m_inFlight;

    bool m_reachable = false;
    int m_failures = 0;
    bool m_hasData = false;
    TrydanRealTimeData m_data;
    QElapsedTimer m_dataClock;
};

#endif // TRYDANCLIENT_H
