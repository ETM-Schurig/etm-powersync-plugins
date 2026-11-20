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

#ifndef TRYDANDISCOVERY_H
#define TRYDANDISCOVERY_H

#include "trydanrealtimedata.h"

#include <QHostAddress>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>

#include <network/networkdevicediscovery.h>

// Finds Trydan chargers on the local network.
//
// The network scan of nymea lists the hosts; each host found gets ONE GET /RealTimeData,
// with a short timeout and no retry. A host is kept only if its reply passes
// TrydanRealTimeData::looksLikeTrydan(). Read only: discovery never writes, and never
// sends a second request to the same host.
class TrydanDiscovery : public QObject
{
    Q_OBJECT
public:
    struct Result {
        QHostAddress address;
        TrydanRealTimeData data;
    };

    static constexpr int ProbeTimeoutMs = 3000;

    explicit TrydanDiscovery(NetworkDeviceDiscovery *networkDeviceDiscovery, QObject *parent = nullptr);

    void start();
    QList<Result> results() const { return m_results; }

signals:
    void finished();

private:
    void probe(const QHostAddress &address);
    void finishWhenDone();

    NetworkDeviceDiscovery *m_networkDeviceDiscovery = nullptr;
    QNetworkAccessManager m_nam;
    QList<QHostAddress> m_probed;
    QList<Result> m_results;
    int m_pending = 0;
    bool m_scanFinished = false;
    bool m_finished = false;
};

#endif // TRYDANDISCOVERY_H
