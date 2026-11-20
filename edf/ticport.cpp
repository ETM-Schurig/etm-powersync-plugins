// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "ticport.h"

TicPort::TicPort(QObject *parent) :
    QObject(parent)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &TicPort::open);
    connect(&m_aliveTimer, &QTimer::timeout, this, &TicPort::checkAlive);
}

TicPort::~TicPort()
{
    m_running = false;
    close();
}

bool TicPort::portOpen() const
{
    return m_port && m_port->isOpen();
}

bool TicPort::looksLikeStandardMode() const
{
    return m_decoder.standardGroups() > 0 && m_decoder.framesDecoded() == 0;
}

void TicPort::start()
{
    m_running = true;
    m_aliveTimer.start(qMax(100, qMin(1000, m_staleAfterMs / 4)));
    open();
}

void TicPort::stop()
{
    m_running = false;
    m_retryTimer.stop();
    m_aliveTimer.stop();
    close();
    setConnected(false);
}

void TicPort::open()
{
    if (!m_running)
        return;
    close();
    m_decoder.clear();

    // The signals are connected once the port is open: a failing open() emits errorOccurred
    // from inside open(), which must not close the port under our feet.
    QSerialPort *port = new QSerialPort(this);
    port->setPortName(m_portName);
    if (!port->open(QIODevice::ReadOnly)) {
        reportError(QStringLiteral("cannot open %1: %2").arg(m_portName, port->errorString()));
        port->deleteLater();
        m_retryTimer.start(m_retryMs);
        return;
    }
    m_port = port;
    connect(m_port, &QSerialPort::readyRead, this, &TicPort::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this, &TicPort::onError);
    bool configured = m_port->setBaudRate(HistoricBaudRate);
    if (m_mode == Mode::SevenEven) {
        configured &= m_port->setDataBits(QSerialPort::Data7);
        configured &= m_port->setParity(QSerialPort::EvenParity);
    } else {
        configured &= m_port->setDataBits(QSerialPort::Data8);
        configured &= m_port->setParity(QSerialPort::NoParity);
    }
    configured &= m_port->setStopBits(QSerialPort::OneStop);
    configured &= m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!configured) {
        // A pseudo-terminal or an odd driver may refuse a setting: keep reading, the
        // checksums say whether what arrives is usable.
        reportError(QStringLiteral("%1 refused a line setting: %2").arg(m_portName, m_port->errorString()));
        m_port->clearError();
    }
    m_openedAt = QDateTime::currentDateTimeUtc();
    m_lastError.clear();
}

void TicPort::close()
{
    if (!m_port)
        return;
    QSerialPort *port = m_port;
    m_port = nullptr;
    port->disconnect(this);
    if (port->isOpen())
        port->close();
    port->deleteLater();
}

void TicPort::onReadyRead()
{
    if (!m_port)
        return;
    const QByteArray bytes = m_port->readAll();
    const QList<TicDecoder::Frame> frames = m_decoder.feed(bytes);
    for (const TicDecoder::Frame &frame : frames) {
        const TicReading reading = TicReading::fromFrame(frame);
        if (reading.isEmpty())
            continue;
        m_lastFrame = QDateTime::currentDateTimeUtc();
        setConnected(true);
        emit frameReceived(reading);
    }
}

void TicPort::onError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || !m_port)
        return;
    // Qt 6 reports no parity or framing error: a corrupted byte reaches the decoder, whose
    // checksums drop the group.
    if (error == QSerialPort::TimeoutError) {
        m_port->clearError();
        return;
    }
    // ResourceError (adapter pulled, pty closed), ReadError, ...: reopen later.
    reportError(QStringLiteral("%1: %2").arg(m_portName, m_port->errorString()));
    close();
    setConnected(false);
    if (m_running)
        m_retryTimer.start(m_retryMs);
}

void TicPort::checkAlive()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_connected && m_lastFrame.msecsTo(now) > m_staleAfterMs)
        setConnected(false);

    // Open but silent for long: reopen. A USB adapter re-enumerated under the same name, or a
    // pseudo-terminal recreated behind a stable link, comes back this way.
    if (portOpen()) {
        const QDateTime reference = m_lastFrame.isValid() && m_lastFrame > m_openedAt ? m_lastFrame : m_openedAt;
        if (reference.msecsTo(now) > m_reopenAfterMs) {
            reportError(QStringLiteral("%1 is silent, reopening").arg(m_portName));
            open();
        }
    }
}

void TicPort::setConnected(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    emit connectedChanged(connected);
}

void TicPort::reportError(const QString &error)
{
    if (error == m_lastError)
        return;
    m_lastError = error;
    emit portError(error);
}
