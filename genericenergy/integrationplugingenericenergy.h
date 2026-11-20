// SPDX-License-Identifier: GPL-3.0-or-later

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
* Copyright (C) 2013 - 2024, nymea GmbH
* Copyright (C) 2024 - 2025, chargebyte austria GmbH
*
* This file is part of nymea-plugins-genericthings.
*
* nymea-plugins-genericthings is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* nymea-plugins-genericthings is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with nymea-plugins-genericthings. If not, see <https://www.gnu.org/licenses/>.
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef INTEGRATIONPLUGINGENERICENERGY_H
#define INTEGRATIONPLUGINGENERICENERGY_H

#include <integrations/integrationplugin.h>
#include <plugintimer.h>

#include "extern-plugininfo.h"

#include <QTimer>
#include <QDateTime>

class IntegrationPluginGenericEnergy: public IntegrationPlugin
{
    Q_OBJECT

    Q_PLUGIN_METADATA(IID "io.nymea.IntegrationPlugin" FILE "integrationplugingenericenergy.json")
    Q_INTERFACES(IntegrationPlugin)

public:
    explicit IntegrationPluginGenericEnergy();
    void setupThing(ThingSetupInfo *info) override;
    void executeAction(ThingActionInfo *info) override;
    void thingRemoved(Thing *thing) override;

private:
    // Integrates currentPower into the energy counters of every class that carries them.
    // See the comment at the definition: these counters were only written by their action.
    void integrerEnergie();
    // True if the class of this Thing is in the integration table.
    bool classeIntegree(const ThingClassId &classe) const;

    QHash<Thing *, int> m_pulsesPerTimeframe;

    QHash<Thing *, QTimer *> m_smartMeterTimer;

    // ── Power → energy integration ─────────────────────────────────────────────────────
    // ONE timer for all classes and all instances: the operation is the same,
    // and one timer per Thing would multiply wake-ups without adding anything.
    PluginTimer *m_energieTimer = nullptr;
    // Timestamp of the last integration, per Thing. The time ACTUALLY elapsed is what
    // gets integrated, never the nominal period of the timer.
    QHash<Thing *, QDateTime> m_derniereIntegration;
    // Last write of the counter by an EXTERNAL source (the action). While it speaks,
    // the integration stays silent — see integrerEnergie().
    QHash<Thing *, QDateTime> m_derniereEcritureExterne;
};

#endif // INTEGRATIONPLUGINGENERICENERGY_H
