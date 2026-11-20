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

#ifndef INTEGRATIONPLUGINEDF_H
#define INTEGRATIONPLUGINEDF_H

#include <integrations/integrationplugin.h>

#include "contract.h"
#include "ejpcalendar.h"
#include "ticport.h"
#include "ticreading.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QTimer>

// Two things:
//  - "Compteur Linky (TIC)": reads the historic TIC on a serial port, publishes what it says.
//    Not an energy meter (PAPP is in VA, there is no export index in historic mode, and a
//    grid meter appearing without a gesture must never become the root meter).
//  - "Contrat d'électricité": the option, the periods and their prices, the nominal price
//    0-100, the next switch, EJP days, and the price slots to the end of tomorrow. It follows
//    a linked Linky (parameter set by the discovery) for the option and the period in progress.
class IntegrationPluginEdf : public IntegrationPlugin
{
    Q_OBJECT

    Q_PLUGIN_METADATA(IID "io.nymea.IntegrationPlugin" FILE "integrationpluginedf.json")
    Q_INTERFACES(IntegrationPlugin)

public:
    explicit IntegrationPluginEdf();

    void discoverThings(ThingDiscoveryInfo *info) override;
    void setupThing(ThingSetupInfo *info) override;
    void executeAction(ThingActionInfo *info) override;
    void thingRemoved(Thing *thing) override;

private:
    struct ContractRuntime {
        Contract contract;
        EjpCalendar calendar;
        QTimer *tick = nullptr;
        QTimer *switchTimer = nullptr;
        QVariantMap lastTransport;   // without generatedAt: republished only when it changes
        QString lastLinkyStatus;
    };

    void setupLinky(ThingSetupInfo *info);
    void setupContract(ThingSetupInfo *info);

    void onTicFrame(Thing *linky, const TicReading &reading);
    void updateLinkyStates(Thing *linky, const TicReading &reading);

    Thing *linkedLinky(Thing *contract) const;
    QString linkStatus(Thing *contract, const ContractRuntime &runtime, QString *ticPeriod) const;
    void updateContract(Thing *contract);
    void saveCalendar(Thing *contract);

    static std::optional<Contract::Type> typeFromParam(const QString &value);
    static Contract::Type typeFromOption(TicReading::Option option, bool *known);

    QHash<Thing *, TicPort *> m_ports;
    QHash<Thing *, TicReading> m_lastReading;
    QHash<Thing *, QDateTime> m_lastReadingAt;
    QHash<Thing *, ContractRuntime> m_contracts;
};

#endif // INTEGRATIONPLUGINEDF_H
