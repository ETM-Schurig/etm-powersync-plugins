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

#include "trydandiscovery.h"
#include "extern-plugininfo.h"

#include <network/networkdevicediscoveryreply.h>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

TrydanDiscovery::TrydanDiscovery(NetworkDeviceDiscovery *networkDeviceDiscovery, QObject *parent)
    : QObject(parent),
      m_networkDeviceDiscovery(networkDeviceDiscovery)
{
}

void TrydanDiscovery::start()
{
    NetworkDeviceDiscoveryReply *reply = m_networkDeviceDiscovery->discover();
    connect(reply, &NetworkDeviceDiscoveryReply::hostAddressDiscovered, this, &TrydanDiscovery::probe);
    connect(reply, &NetworkDeviceDiscoveryReply::finished, reply, &QObject::deleteLater);
    connect(reply, &NetworkDeviceDiscoveryReply::finished, this, [this, reply]() {
        // Hosts that were already in the cache may only be listed here.
        for (const NetworkDeviceInfo &info : reply->networkDeviceInfos())
            probe(info.address());
        m_scanFinished = true;
        // Probes still running get their own timeout; this is the bound on the whole.
        QTimer::singleShot(ProbeTimeoutMs + 500, this, [this]() {
            m_pending = 0;
            finishWhenDone();
        });
        finishWhenDone();
    });
}

void TrydanDiscovery::probe(const QHostAddress &address)
{
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol || m_probed.contains(address))
        return;
    m_probed.append(address);

    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(address.toString());
    url.setPath(QStringLiteral("/RealTimeData"));
    QNetworkRequest request(url);
    request.setTransferTimeout(ProbeTimeoutMs);

    ++m_pending;
    QNetworkReply *reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, address]() {
        reply->deleteLater();
        if (m_pending > 0)
            --m_pending;
        if (reply->error() == QNetworkReply::NoError) {
            TrydanRealTimeData data;
            if (TrydanRealTimeData::parse(reply->readAll(), &data) && data.looksLikeTrydan()) {
                qCDebug(dcTrydan()) << "Discovery: Trydan found at" << address.toString()
                                    << "firmware" << data.firmwareVersion;
                m_results.append({address, data});
            }
        }
        finishWhenDone();
    });
}

void TrydanDiscovery::finishWhenDone()
{
    if (m_finished || !m_scanFinished || m_pending > 0)
        return;
    m_finished = true;
    qCInfo(dcTrydan()) << "Discovery finished:" << m_results.count() << "Trydan(s) among"
                       << m_probed.count() << "hosts probed.";
    emit finished();
}
