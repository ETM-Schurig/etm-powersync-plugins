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

#include "trydanclient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

void TrydanReply::finish(Error error, const QString &errorString)
{
    if (m_finished)
        return;
    m_finished = true;
    m_error = error;
    m_errorString = errorString;
    emit finished();
}

TrydanClient::TrydanClient(const QString &host, quint16 port, QObject *parent)
    : QObject(parent),
      m_host(host),
      m_port(port)
{
    m_pollTimer.setInterval(DefaultPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() {
        // Never stack polls: anything queued or running will refresh the data anyway.
        if (m_busy || !m_queue.isEmpty())
            return;
        enqueue(Job());
    });
}

TrydanClient::~TrydanClient()
{
    m_pollTimer.stop();
    if (m_inFlight) {
        m_inFlight->disconnect(this);
        m_inFlight->abort();
    }
    QList<QPointer<TrydanReply>> replies = m_current.replies;
    for (const Job &job : std::as_const(m_queue))
        replies += job.replies;
    for (const QPointer<TrydanReply> &reply : std::as_const(replies)) {
        if (reply)
            reply->finish(TrydanReply::Aborted, QStringLiteral("client destroyed"));
    }
}

void TrydanClient::setHost(const QString &host)
{
    m_host = host;
}

void TrydanClient::setPollInterval(int ms)
{
    m_pollTimer.setInterval(ms);
}

void TrydanClient::setVerify(int attempts, int delayMs)
{
    m_verifyAttempts = qMax(1, attempts);
    m_verifyDelayMs = qMax(0, delayMs);
}

void TrydanClient::startPolling()
{
    m_pollTimer.start();
}

void TrydanClient::stopPolling()
{
    m_pollTimer.stop();
}

qint64 TrydanClient::dataAgeMs() const
{
    return m_hasData ? m_dataClock.elapsed() : -1;
}

int TrydanClient::pending() const
{
    return m_queue.count() + (m_busy ? 1 : 0);
}

TrydanReply *TrydanClient::refresh()
{
    TrydanReply *reply = new TrydanReply(this);
    connect(reply, &TrydanReply::finished, reply, &QObject::deleteLater, Qt::QueuedConnection);

    // A read already running (not one verifying a write: that one answers another question)
    // or already queued gives the same fresh data.
    if (m_busy && m_current.type == JobType::Read) {
        m_current.replies.append(reply);
        return reply;
    }
    for (Job &job : m_queue) {
        if (job.type == JobType::Read) {
            job.replies.append(reply);
            return reply;
        }
    }
    Job job;
    job.replies.append(reply);
    enqueue(job);
    return reply;
}

TrydanReply *TrydanClient::write(const QString &param, int value)
{
    TrydanReply *reply = new TrydanReply(this);
    connect(reply, &TrydanReply::finished, reply, &QObject::deleteLater, Qt::QueuedConnection);

    Job job;
    job.type = JobType::Write;
    job.param = param;
    job.value = value;
    job.attemptsLeft = m_verifyAttempts;
    job.replies.append(reply);
    enqueue(job);
    return reply;
}

TrydanReply *TrydanClient::setChargingCurrent(double requested)
{
    TrydanReply *reply = new TrydanReply(this);
    connect(reply, &TrydanReply::finished, reply, &QObject::deleteLater, Qt::QueuedConnection);

    Job job;
    job.type = JobType::ChargingCurrent;
    job.requested = requested;
    job.attemptsLeft = m_verifyAttempts;
    job.replies.append(reply);
    enqueue(job);
    return reply;
}

void TrydanClient::enqueue(const Job &job)
{
    m_queue.append(job);
    // Started from the event loop, so that no reply ever finishes inside the call that
    // created it.
    QMetaObject::invokeMethod(this, &TrydanClient::startNext, Qt::QueuedConnection);
}

void TrydanClient::startNext()
{
    if (m_busy || m_queue.isEmpty())
        return;
    Job job = m_queue.takeFirst();
    if (job.type != JobType::Read) {
        bool wanted = false;
        for (const QPointer<TrydanReply> &reply : std::as_const(job.replies))
            wanted = wanted || (reply && !reply->isAbandoned());
        if (!wanted) {
            for (const QPointer<TrydanReply> &reply : std::as_const(job.replies)) {
                if (reply)
                    reply->finish(TrydanReply::Aborted, QStringLiteral("abandoned before it was sent"));
            }
            QMetaObject::invokeMethod(this, &TrydanClient::startNext, Qt::QueuedConnection);
            return;
        }
    }
    m_busy = true;
    m_current = job;
    if (m_current.type == JobType::Read) {
        sendRead();
        return;
    }

    if (m_current.type == JobType::ChargingCurrent) {
        const TrydanModel::CurrentCommand command = TrydanModel::currentCommand(m_data, m_current.requested);
        if (command.kind == TrydanModel::CurrentCommand::Nothing) {
            finishCurrent(TrydanReply::NoError, QString());
            return;
        }
        m_current.type = JobType::Write;
        if (command.kind == TrydanModel::CurrentCommand::Pause) {
            m_current.param = QStringLiteral("Paused");
            m_current.value = 1;
        } else {
            m_current.param = QStringLiteral("Intensity");
            m_current.value = command.ampere;
        }
    }

    // The charger stores every write, and the energy manager re-sends unchanged values.
    if (m_hasData && m_data.writableValue(m_current.param) == m_current.value) {
        finishCurrent(TrydanReply::NoError, QString());
        return;
    }
    for (const QPointer<TrydanReply> &reply : std::as_const(m_current.replies)) {
        if (reply)
            reply->m_written = QStringLiteral("%1=%2").arg(m_current.param).arg(m_current.value);
    }
    sendWrite();
}

QNetworkReply *TrydanClient::get(const QString &path)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_host);
    if (m_port != 80)
        url.setPort(m_port);
    url.setPath(path);

    QNetworkRequest request(url);
    request.setTransferTimeout(m_requestTimeoutMs);
    QNetworkReply *reply = m_nam.get(request);
    m_inFlight = reply;
    return reply;
}

void TrydanClient::sendRead()
{
    QNetworkReply *reply = get(QStringLiteral("/RealTimeData"));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onReadFinished(reply); });
}

void TrydanClient::sendWrite()
{
    QNetworkReply *reply = get(QStringLiteral("/write/%1=%2").arg(m_current.param).arg(m_current.value));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onWriteFinished(reply); });
}

void TrydanClient::onReadFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_inFlight = nullptr;

    const QString what = m_current.verifying
            ? QStringLiteral("read back of %1=%2").arg(m_current.param).arg(m_current.value)
            : QStringLiteral("RealTimeData");

    if (reply->error() != QNetworkReply::NoError) {
        recordFailure(what, reply->errorString());
        finishCurrent(TrydanReply::NetworkError, reply->errorString());
        return;
    }

    TrydanRealTimeData data;
    QString parseError;
    if (!TrydanRealTimeData::parse(reply->readAll(), &data, &parseError)) {
        // Something answered, but not with RealTimeData: as useless as no answer.
        recordFailure(what, QStringLiteral("invalid reply: ") + parseError);
        finishCurrent(TrydanReply::NetworkError, QStringLiteral("invalid reply: ") + parseError);
        return;
    }

    recordAnswer();
    m_data = data;
    m_hasData = true;
    m_dataClock.start();
    setReachable(true);
    emit dataReceived(m_data);

    if (!m_current.verifying) {
        finishCurrent(TrydanReply::NoError, QString());
        return;
    }

    const std::optional<int> readBack = m_data.writableValue(m_current.param);
    if (!readBack) {
        finishCurrent(TrydanReply::Unsupported,
                      QStringLiteral("the charger does not report %1").arg(m_current.param));
        return;
    }
    if (*readBack == m_current.value) {
        finishCurrent(TrydanReply::NoError, QString());
        return;
    }
    if (--m_current.attemptsLeft > 0) {
        // Hold the queue: nothing else goes to the charger until this gesture is decided.
        QTimer::singleShot(m_verifyDelayMs, this, &TrydanClient::sendRead);
        return;
    }
    finishCurrent(TrydanReply::NotTaken,
                  QStringLiteral("%1 reads %2 after writing %3")
                  .arg(m_current.param).arg(*readBack).arg(m_current.value));
}

void TrydanClient::onWriteFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_inFlight = nullptr;

    const QString what = QStringLiteral("write %1=%2").arg(m_current.param).arg(m_current.value);
    if (reply->error() != QNetworkReply::NoError) {
        recordFailure(what, reply->errorString());
        finishCurrent(TrydanReply::NetworkError, reply->errorString());
        return;
    }

    recordAnswer();
    const QByteArray body = reply->readAll().trimmed();
    if (body != "OK") {
        const QString text = body.isEmpty() ? QStringLiteral("empty reply") : QString::fromUtf8(body.left(64));
        emit exchangeFailed(what, QStringLiteral("refused: ") + text);
        finishCurrent(TrydanReply::Rejected, QStringLiteral("the charger answered \"%1\"").arg(text));
        return;
    }

    m_current.verifying = true;
    sendRead();
}

void TrydanClient::finishCurrent(TrydanReply::Error error, const QString &errorString)
{
    const QList<QPointer<TrydanReply>> replies = m_current.replies;
    m_current = Job();
    m_busy = false;
    for (const QPointer<TrydanReply> &reply : replies) {
        if (reply)
            reply->finish(error, errorString);
    }
    QMetaObject::invokeMethod(this, &TrydanClient::startNext, Qt::QueuedConnection);
}

void TrydanClient::recordFailure(const QString &what, const QString &error)
{
    ++m_failures;
    emit exchangeFailed(what, error);
    if (m_failures >= m_errorLimit)
        setReachable(false);
}

void TrydanClient::recordAnswer()
{
    m_failures = 0;
}

void TrydanClient::setReachable(bool reachable)
{
    if (m_reachable == reachable)
        return;
    m_reachable = reachable;
    emit reachableChanged(reachable);
}
