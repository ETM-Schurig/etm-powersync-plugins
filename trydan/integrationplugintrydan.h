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

#ifndef INTEGRATIONPLUGINTRYDAN_H
#define INTEGRATIONPLUGINTRYDAN_H

#include <integrations/integrationplugin.h>

#include "trydanclient.h"
#include "trydanenergy.h"

#include <QDateTime>
#include <QHash>

// V2C Trydan over its local HTTP API.
//
// The plugin exposes simple gestures - pause / resume, setpoint, phase mode, fresh read -
// each one written, read back and reported as taken or not. It does not chain them: the
// 1p/3p switching sequence (down to 6 A, pause, switch, resume) belongs to the energy
// manager, for every kind of charger.
class IntegrationPluginTrydan : public IntegrationPlugin
{
    Q_OBJECT

    Q_PLUGIN_METADATA(IID "io.nymea.IntegrationPlugin" FILE "integrationplugintrydan.json")
    Q_INTERFACES(IntegrationPlugin)

public:
    explicit IntegrationPluginTrydan();

    void discoverThings(ThingDiscoveryInfo *info) override;
    void setupThing(ThingSetupInfo *info) override;
    void executeAction(ThingActionInfo *info) override;
    void thingRemoved(Thing *thing) override;

private:
    void updateStates(Thing *thing, const TrydanRealTimeData &data);
    void updateEnergy(Thing *thing, const TrydanRealTimeData &data, bool pluggedIn);
    void applyPhaseStates(Thing *thing, const TrydanRealTimeData &data);
    void manageInternalOptimizer(Thing *thing, const TrydanRealTimeData &data);
    void setDisconnected(Thing *thing);
    void runGesture(ThingActionInfo *info, TrydanReply *reply, const QString &action);
    uint wiredPhases(Thing *thing) const;
    void storeSessionOpen(Thing *thing, bool open);

    QHash<Thing *, TrydanClient *> m_clients;
    QHash<Thing *, TrydanEnergy> m_energy;
    QHash<Thing *, QDateTime> m_lastPauseDynamicWrite;
    QHash<Thing *, bool> m_wiringWarned;
    QHash<Thing *, TrydanModel::WiringCheck> m_wiringCheck; // last verdict logged
};

#endif // INTEGRATIONPLUGINTRYDAN_H
