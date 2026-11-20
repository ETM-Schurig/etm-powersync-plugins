// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef TICPORT_H
#define TICPORT_H

#include "ticdecoder.h"
#include "ticreading.h"

#include <QDateTime>
#include <QObject>
#include <QSerialPort>
#include <QTimer>

// Reads the TIC on a serial port (UART behind an optocoupler, or a USB serial adapter).
//
// "Connected" means: valid frames received recently - not "port open". A port that cannot be
// opened, disappears (USB adapter pulled) or goes silent (meter wire cut) gives
// connected = false, and the port is reopened every retry interval, without end and without
// logging more than once per outage (the caller logs on connectedChanged).
class TicPort : public QObject
{
    Q_OBJECT
public:
    enum class Mode {
        // 8 data bits, no parity, bit 7 masked by the decoder. The default: it works on every
        // UART, including the Raspberry Pi mini-UART (Pi 3, Zero 2 W /dev/serial0 with
        // Bluetooth on), which has no parity: there the parity bit arrives as bit 7.
        EightNoneMasked,
        // 7 data bits, even parity, 1 stop: what the meter sends. Needs a UART with parity
        // (PL011, USB adapters).
        SevenEven
    };

    explicit TicPort(QObject *parent = nullptr);
    ~TicPort() override;

    void setPortName(const QString &portName) { m_portName = portName; }
    QString portName() const { return m_portName; }
    void setMode(Mode mode) { m_mode = mode; }
    Mode mode() const { return m_mode; }

    // Timings (ms). Tests shorten them.
    void setStaleAfterMs(int ms) { m_staleAfterMs = ms; }  // no valid frame for so long: disconnected
    void setReopenAfterMs(int ms) { m_reopenAfterMs = ms; } // silent for so long: close and reopen
    void setRetryMs(int ms) { m_retryMs = ms; }             // port could not be opened: try again after

    void start();
    void stop();

    bool connected() const { return m_connected; }
    bool portOpen() const;
    QString lastError() const { return m_lastError; }
    const TicDecoder &decoder() const { return m_decoder; }
    // Standard-mode groups seen and no historic frame: the meter is in standard mode.
    bool looksLikeStandardMode() const;

    static constexpr int HistoricBaudRate = 1200;
    static constexpr int StandardBaudRate = 9600; // standard mode: not decoded yet

signals:
    void frameReceived(const TicReading &reading);
    void connectedChanged(bool connected);
    void portError(const QString &error);  // open failed, port lost: once per transition

private:
    void open();
    void close();
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);
    void checkAlive();
    void setConnected(bool connected);
    void reportError(const QString &error);

    QString m_portName;
    Mode m_mode = Mode::EightNoneMasked;
    QSerialPort *m_port = nullptr;
    TicDecoder m_decoder;
    QTimer m_retryTimer;
    QTimer m_aliveTimer;
    QDateTime m_lastFrame;
    QDateTime m_openedAt;
    bool m_connected = false;
    bool m_running = false;
    QString m_lastError;
    int m_staleAfterMs = 10000;
    int m_reopenAfterMs = 60000;
    int m_retryMs = 10000;
};

#endif // TICPORT_H
