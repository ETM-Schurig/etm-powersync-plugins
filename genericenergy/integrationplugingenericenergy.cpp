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

#include "integrationplugingenericenergy.h"
#include "plugininfo.h"

#include <QDebug>
#include <QtMath>
#include <QDateTime>

namespace {
// How long the integration stays silent after an external write of a counter.
// It must cover the period of the slowest source we want to respect - and it
// EXPIRES, so that a one-off reset does not freeze the counter for good.
const int SuspensionSourceExterneS = 15 * 60;

// ── INTEGRATION TABLE ─────────────────────────────────────────────────────────────────────
//
// Four classes carried an energy counter that NOBODY wrote outside its action:
// a value set once stayed there forever. One table rather than four copies -
// the operation is the same, only the states to read and to credit change.
//
// THE SIGN DECIDES WHICH QUANTITY IS CREDITED, and this is not a local convention: the nymea
// `smartmeterproducer` interface states it plainly - "currentPower is always from a
// consumer perspective. Thus it will be a negative value for producing devices." A
// NEGATIVE power therefore credits the PRODUCED counter, never the consumed one, and the
// count uses its absolute value.
//
// A NULL state id means the class does not carry that counter - a producer has no
// totalEnergyConsumed. That direction then credits nothing, rather than inventing a
// missing state.
struct RegleIntegration {
    ThingClassId classe;
    StateTypeId  puissance;
    StateTypeId  consomme;   // null if the class does not carry this counter
    StateTypeId  produit;    // null if the class does not carry this counter
};

QVector<RegleIntegration> reglesIntegration()
{
    return {
        { smartMeterConsumerThingClassId, smartMeterConsumerCurrentPowerStateTypeId,
          smartMeterConsumerTotalEnergyConsumedStateTypeId, StateTypeId() },
        { smartMeterProducerThingClassId, smartMeterProducerCurrentPowerStateTypeId,
          StateTypeId(), smartMeterProducerTotalEnergyProducedStateTypeId },
        // The only BIDIRECTIONAL meter of the table: the same power state feeds
        // one counter or the other depending on its sign.
        { energyMeterThingClassId, energyMeterCurrentPowerStateTypeId,
          energyMeterTotalEnergyConsumedStateTypeId, energyMeterTotalEnergyProducedStateTypeId },
        { wallboxThingClassId, wallboxCurrentPowerStateTypeId,
          wallboxTotalEnergyConsumedStateTypeId, StateTypeId() },
    };
}
// impulseSmartMeter is ABSENT, on purpose: its counter is already kept, impulse by
// impulse, in its own handler. Integrating it as well would count it twice.
// energyStorage and socket are absent, having no counter to feed.
}

IntegrationPluginGenericEnergy::IntegrationPluginGenericEnergy()
{

}

void IntegrationPluginGenericEnergy::setupThing(ThingSetupInfo *info)
{
    Thing *thing = info->thing();

    if (thing->thingClassId() == impulseSmartMeterThingClassId) {

        QTimer* smartMeterTimer = new QTimer(this);
        int timeframe = thing->setting(impulseSmartMeterSettingsImpulseTimeframeParamTypeId).toInt();
        smartMeterTimer->setInterval(timeframe * 1000);
        m_smartMeterTimer.insert(thing, smartMeterTimer);
        smartMeterTimer->start();
        connect(thing, &Thing::settingChanged, smartMeterTimer, [smartMeterTimer] (const ParamTypeId &paramTypeId, const QVariant &value) {
            if (paramTypeId == impulseSmartMeterSettingsImpulseTimeframeParamTypeId) {
                smartMeterTimer->setInterval(value.toInt() * 1000);
            }
        });

        connect(smartMeterTimer, &QTimer::timeout, thing, [this, smartMeterTimer, thing] {
            double impulsePerKwh = thing->setting(impulseSmartMeterSettingsImpulsePerKwhParamTypeId).toDouble();
            int interval = smartMeterTimer->interval()/1000;
            double power = (m_pulsesPerTimeframe.value(thing)/impulsePerKwh)/(interval/3600.00); // Power = Energy/Time; Energy = Impulses/ImpPerkWh
            thing->setStateValue(impulseSmartMeterCurrentPowerStateTypeId, power*1000);
            m_pulsesPerTimeframe.insert(thing, 0);
        });
    } else if  (thing->thingClassId() == energyStorageThingClassId) {
        connect(thing, &Thing::settingChanged, [thing](const ParamTypeId &settingTypeId, const QVariant &value){
            if (settingTypeId == energyStorageSettingsCapacityParamTypeId) {
                thing->setStateValue(energyStorageCapacityStateTypeId, value);
            }
            if (settingTypeId == energyStorageSettingsCriticalLevelParamTypeId) {
                int currentBatteryLevel = thing->stateValue(energyStorageBatteryLevelStateTypeId).toInt();
                thing->setStateValue(energyStorageBatteryCriticalStateTypeId, currentBatteryLevel <= value.toInt());
            }
        });
    } else if (thing->thingClassId() == wallboxThingClassId) {
        connect(thing, &Thing::settingChanged, [thing](const ParamTypeId &settingTypeId, const QVariant &value) {
            if (settingTypeId == wallboxSettingsMinChargingCurrentParamTypeId) {
                thing->setStateMinValue(wallboxMaxChargingCurrentStateTypeId, value);
            } else if (settingTypeId == wallboxSettingsMaxChargingCurrentParamTypeId) {
                thing->setStateMaxValue(wallboxMaxChargingCurrentStateTypeId, value);
            }
        });
    }

    if (classeIntegree(thing->thingClassId())) {
        // These counters accumulated NOTHING: their total was only written by its action, so
        // a value set once stayed there forever. A frozen counter is not a missing counter -
        // it reads as a valid measurement, and everything that integrates it (progress of a
        // charge, energy balance) concludes "nothing consumed" without any error showing
        // anywhere.
        //
        // On a nymead RESTART: the state is `cached` by default (StateType::m_cached = true,
        // types/statetype.h), so the counter RESUMES from its value - it does not restart at
        // zero, which would make a monotonic quantity go backwards. The integration timestamp,
        // however, restarts from HERE: the downtime is not billed as energy.
        m_derniereIntegration.insert(thing, QDateTime::currentDateTime());
        if (!m_energieTimer) {
            m_energieTimer = hardwareManager()->pluginTimerManager()->registerTimer(1);
            connect(m_energieTimer, &PluginTimer::timeout, this, &IntegrationPluginGenericEnergy::integrerEnergie);
        }
    }


    // Fall trough, if not already finished and returned...
    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginGenericEnergy::executeAction(ThingActionInfo *info)
{
    Thing *thing = info->thing();
    Action action = info->action();

    if (thing->thingClassId() == socketThingClassId) {
        if (action.actionTypeId() == socketPowerActionTypeId) {
            thing->setStateValue(socketPowerStateTypeId, action.param(socketPowerActionPowerParamTypeId).value());
            return info->finish(Thing::ThingErrorNoError);
        } else {
            Q_ASSERT_X(false, "executeAction", QString("Unhandled actionTypeId: %1").arg(action.actionTypeId().toString()).toUtf8());
        }
    } else if (thing->thingClassId() == energyMeterThingClassId) {
        if (action.actionTypeId() == energyMeterCurrentPowerActionTypeId) {
            thing->setStateValue(energyMeterCurrentPowerStateTypeId,
                                 action.paramValue(energyMeterCurrentPowerActionCurrentPowerParamTypeId));
        } else if (action.actionTypeId() == energyMeterTotalEnergyConsumedActionTypeId) {
            thing->setStateValue(energyMeterTotalEnergyConsumedStateTypeId,
                                 action.paramValue(energyMeterTotalEnergyConsumedActionTotalEnergyConsumedParamTypeId));
            // Someone else keeps this counter: the integration leaves it to them.
            m_derniereEcritureExterne.insert(thing, QDateTime::currentDateTime());
        } else if (action.actionTypeId() == energyMeterTotalEnergyProducedActionTypeId) {
            thing->setStateValue(energyMeterTotalEnergyProducedStateTypeId,
                                 action.paramValue(energyMeterTotalEnergyProducedActionTotalEnergyProducedParamTypeId));
            // Someone else keeps this counter: the integration leaves it to them.
            m_derniereEcritureExterne.insert(thing, QDateTime::currentDateTime());
        } else if (action.actionTypeId() == energyMeterCurrentPowerPhaseAActionTypeId) {
            thing->setStateValue(energyMeterCurrentPowerPhaseAStateTypeId,
                                 action.paramValue(energyMeterCurrentPowerPhaseAActionCurrentPowerPhaseAParamTypeId));
        } else if (action.actionTypeId() == energyMeterCurrentPowerPhaseBActionTypeId) {
            thing->setStateValue(energyMeterCurrentPowerPhaseBStateTypeId,
                                 action.paramValue(energyMeterCurrentPowerPhaseBActionCurrentPowerPhaseBParamTypeId));
        } else if (action.actionTypeId() == energyMeterCurrentPowerPhaseCActionTypeId) {
            thing->setStateValue(energyMeterCurrentPowerPhaseCStateTypeId,
                                 action.paramValue(energyMeterCurrentPowerPhaseCActionCurrentPowerPhaseCParamTypeId));
        } else if (action.actionTypeId() == energyMeterCurrentPhaseAActionTypeId) {
            thing->setStateValue(energyMeterCurrentPhaseAStateTypeId, action.paramValue(energyMeterCurrentPhaseAActionCurrentPhaseAParamTypeId));
        } else if (action.actionTypeId() == energyMeterCurrentPhaseBActionTypeId) {
            thing->setStateValue(energyMeterCurrentPhaseBStateTypeId, action.paramValue(energyMeterCurrentPhaseBActionCurrentPhaseBParamTypeId));
        } else if (action.actionTypeId() == energyMeterCurrentPhaseCActionTypeId) {
            thing->setStateValue(energyMeterCurrentPhaseCStateTypeId, action.paramValue(energyMeterCurrentPhaseCActionCurrentPhaseCParamTypeId));
        } else if (action.actionTypeId() == energyMeterVoltagePhaseAActionTypeId) {
            thing->setStateValue(energyMeterVoltagePhaseAStateTypeId, action.paramValue(energyMeterVoltagePhaseAActionVoltagePhaseAParamTypeId));
        } else if (action.actionTypeId() == energyMeterVoltagePhaseBActionTypeId) {
            thing->setStateValue(energyMeterVoltagePhaseBStateTypeId, action.paramValue(energyMeterVoltagePhaseBActionVoltagePhaseBParamTypeId));
        } else if (action.actionTypeId() == energyMeterVoltagePhaseCActionTypeId) {
            thing->setStateValue(energyMeterVoltagePhaseCStateTypeId, action.paramValue(energyMeterVoltagePhaseCActionVoltagePhaseCParamTypeId));
        }
        info->finish(Thing::ThingErrorNoError);
    } else if (thing->thingClassId() == impulseSmartMeterThingClassId) {
        if (action.actionTypeId() == impulseSmartMeterImpulseInputActionTypeId) {
            bool value = info->action().param(impulseSmartMeterImpulseInputActionImpulseInputParamTypeId).value().toBool();
            thing->setStateValue(impulseSmartMeterImpulseInputStateTypeId, value);
            int impulsePerKwh = info->thing()->setting(impulseSmartMeterSettingsImpulsePerKwhParamTypeId).toInt();
            if (value) {
                double currentEnergy = thing->stateValue(impulseSmartMeterTotalEnergyConsumedStateTypeId).toDouble();
                thing->setStateValue(impulseSmartMeterTotalEnergyConsumedStateTypeId ,currentEnergy + (1.00/impulsePerKwh));
                m_pulsesPerTimeframe[thing]++;
            }
            info->finish(Thing::ThingErrorNoError);
            return;
        } else {
            Q_ASSERT_X(false, "executeAction", QString("Unhandled actionTypeId: %1").arg(action.actionTypeId().toString()).toUtf8());
        }

    } else if (thing->thingClassId() == smartMeterConsumerThingClassId) {
        if (action.actionTypeId() == smartMeterConsumerCurrentPowerActionTypeId) {
            thing->setStateValue(smartMeterConsumerCurrentPowerStateTypeId, action.paramValue(smartMeterConsumerCurrentPowerActionCurrentPowerParamTypeId));
            info->finish(Thing::ThingErrorNoError);
        } else if (action.actionTypeId() == smartMeterConsumerTotalEnergyConsumedActionTypeId) {
            thing->setStateValue(smartMeterConsumerTotalEnergyConsumedStateTypeId, action.paramValue(smartMeterConsumerTotalEnergyConsumedActionTotalEnergyConsumedParamTypeId));
            // Someone else keeps this counter: the integration leaves it to them.
            m_derniereEcritureExterne.insert(thing, QDateTime::currentDateTime());
            info->finish(Thing::ThingErrorNoError);
        } else {
            Q_ASSERT_X(false, "executeAction", QString("Unhandled actionTypeId: %1").arg(action.actionTypeId().toString()).toUtf8());
        }

    } else if (thing->thingClassId() == smartMeterProducerThingClassId) {
        if (action.actionTypeId() == smartMeterProducerCurrentPowerActionTypeId) {
            thing->setStateValue(smartMeterProducerCurrentPowerStateTypeId, action.paramValue(smartMeterProducerCurrentPowerActionCurrentPowerParamTypeId));
            info->finish(Thing::ThingErrorNoError);
        } else if (action.actionTypeId() == smartMeterProducerTotalEnergyProducedActionTypeId) {
            thing->setStateValue(smartMeterProducerTotalEnergyProducedStateTypeId, action.paramValue(smartMeterProducerTotalEnergyProducedActionTotalEnergyProducedParamTypeId));
            // Someone else keeps this counter: the integration leaves it to them.
            m_derniereEcritureExterne.insert(thing, QDateTime::currentDateTime());
            info->finish(Thing::ThingErrorNoError);
        } else {
            Q_ASSERT_X(false, "executeAction", QString("Unhandled actionTypeId: %1").arg(action.actionTypeId().toString()).toUtf8());
        }
    } else if (thing->thingClassId() == energyStorageThingClassId) {
        if (action.actionTypeId() == energyStorageBatteryLevelActionTypeId) {
            int value = action.paramValue(energyStorageBatteryLevelActionBatteryLevelParamTypeId).toInt();
            thing->setStateValue(energyStorageBatteryLevelStateTypeId, value);
            int criticalValue = thing->setting(energyStorageSettingsCriticalLevelParamTypeId).toInt();
            thing->setStateValue(energyStorageBatteryCriticalStateTypeId, value <= criticalValue);
            info->finish(Thing::ThingErrorNoError);
        } else if (action.actionTypeId() == energyStorageCurrentPowerActionTypeId) {
            thing->setStateValue(energyStorageCurrentPowerStateTypeId, action.paramValue(energyStorageCurrentPowerActionCurrentPowerParamTypeId));
            info->finish(Thing::ThingErrorNoError);
        } else if (action.actionTypeId() == energyStorageChargingActionTypeId) {
            thing->setStateValue(energyStorageChargingStateTypeId, action.paramValue(energyStorageChargingActionChargingParamTypeId));
            info->finish(Thing::ThingErrorNoError);
            return;
        }
    } else if (thing->thingClassId() == wallboxThingClassId) {
        if (action.actionTypeId() == wallboxChargingActionTypeId) {
            thing->setStateValue(wallboxChargingStateTypeId, action.paramValue(wallboxChargingActionChargingParamTypeId).toBool());
        } else if (action.actionTypeId() == wallboxCurrentPowerActionTypeId) {
            thing->setStateValue(wallboxCurrentPowerStateTypeId, action.paramValue(wallboxCurrentPowerActionCurrentPowerParamTypeId).toDouble());
        } else if (action.actionTypeId() == wallboxDesiredPhaseCountActionTypeId) {
            thing->setStateValue(wallboxDesiredPhaseCountStateTypeId, action.paramValue(wallboxDesiredPhaseCountActionDesiredPhaseCountParamTypeId).toUInt());
        } else if (action.actionTypeId() == wallboxMaxChargingCurrentActionTypeId) {
            thing->setStateValue(wallboxMaxChargingCurrentStateTypeId, action.paramValue(wallboxMaxChargingCurrentActionMaxChargingCurrentParamTypeId).toDouble());
        } else if (action.actionTypeId() == wallboxPhaseCountActionTypeId) {
            thing->setStateValue(wallboxPhaseCountStateTypeId, action.paramValue(wallboxPhaseCountActionPhaseCountParamTypeId).toUInt());
        } else if (action.actionTypeId() == wallboxPluggedInActionTypeId) {
            thing->setStateValue(wallboxPluggedInStateTypeId, action.paramValue(wallboxPluggedInActionPluggedInParamTypeId).toBool());
        } else if (action.actionTypeId() == wallboxPowerActionTypeId) {
            thing->setStateValue(wallboxPowerStateTypeId, action.paramValue(wallboxPowerActionPowerParamTypeId).toBool());
        } else if (action.actionTypeId() == wallboxTotalEnergyConsumedActionTypeId) {
            thing->setStateValue(wallboxTotalEnergyConsumedStateTypeId, action.paramValue(wallboxTotalEnergyConsumedActionTotalEnergyConsumedParamTypeId).toDouble());
            // Someone else keeps this counter: the integration leaves it to them.
            m_derniereEcritureExterne.insert(thing, QDateTime::currentDateTime());
        } else {
            Q_ASSERT_X(false, "Generic Wallbox", QString("Unhandled action: %1").arg(action.actionTypeId().toString()).toUtf8());
            info->finish(Thing::ThingErrorUnsupportedFeature);
            return;
        }
        info->finish(Thing::ThingErrorNoError);

    } else {
        Q_ASSERT_X(false, "executeAction", QString("Unhandled thingClassId: %1").arg(thing->thingClassId().toString()).toUtf8());
    }
}

bool IntegrationPluginGenericEnergy::classeIntegree(const ThingClassId &classe) const
{
    const QVector<RegleIntegration> regles = reglesIntegration();
    for (const RegleIntegration &r : regles)
        if (r.classe == classe)
            return true;
    return false;
}

void IntegrationPluginGenericEnergy::integrerEnergie()
{
    const QDateTime maintenant = QDateTime::currentDateTime();
    const QVector<RegleIntegration> regles = reglesIntegration();

    for (const RegleIntegration &regle : regles) {
        foreach (Thing *thing, myThings().filterByThingClassId(regle.classe)) {
            const QDateTime avant = m_derniereIntegration.value(thing, maintenant);
            m_derniereIntegration.insert(thing, maintenant);

            // DEFERENCE. These classes are meant to be fed from outside: if a source writes
            // a counter itself, incrementing it as well would corrupt it. We stay silent while
            // it speaks - but the suspension EXPIRES, otherwise a one-off reset would switch
            // the integration off for good and recreate the frozen counter fixed here.
            // It does not survive a restart: a periodic source re-arms it by itself on its
            // first write.
            //
            // The suspension applies to the THING, not to the state: a source that keeps one
            // of the two counters of a bidirectional meter keeps both.
            const QDateTime externe = m_derniereEcritureExterne.value(thing);
            if (externe.isValid() && externe.secsTo(maintenant) < SuspensionSourceExterneS)
                continue;

            // The time ACTUALLY elapsed, never the nominal period: a late or caught-up tick
            // would skew the counter by an amount nobody could name afterwards.
            const double heures = avant.msecsTo(maintenant) / 3600000.0;
            if (heures <= 0)
                continue;

            const double puissanceW = thing->stateValue(regle.puissance).toDouble();

            // The SIGN picks the credited quantity (see table). Each counter is monotonic:
            // it moves forward or stays still, it never goes back. Making it go back would
            // produce exactly the case a real meter never produces, and that the engine treats
            // as an anomaly.
            StateTypeId cible;
            double watts = 0.0;
            if (puissanceW > 0) {
                cible  = regle.consomme;
                watts  = puissanceW;
            } else if (puissanceW < 0) {
                cible  = regle.produit;
                watts  = -puissanceW;
            }
            // No counter for this direction (a producer has no totalEnergyConsumed):
            // the energy is credited nowhere rather than filed under the other one.
            if (cible.isNull() || watts <= 0)
                continue;

            const double cumulKWh = thing->stateValue(cible).toDouble();
            thing->setStateValue(cible, cumulKWh + watts * heures / 1000.0);
        }
    }
}

void IntegrationPluginGenericEnergy::thingRemoved(Thing *thing)
{
    if (thing->thingClassId() == impulseSmartMeterThingClassId) {
        // The two original branches tested the SAME class: the second was unreachable,
        // and it was the one destroying the QTimer. Every removal of an impulse meter
        // therefore left behind an active timer and a table entry - a timer that
        // keeps computing a power for a Thing that no longer exists.
        if (QTimer *timer = m_smartMeterTimer.take(thing))
            timer->deleteLater();
        m_pulsesPerTimeframe.remove(thing);
    }

    if (classeIntegree(thing->thingClassId())) {
        // Both tables are keyed by pointer: not purging them would leave a dangling
        // pointer that the next tick would dereference.
        m_derniereIntegration.remove(thing);
        m_derniereEcritureExterne.remove(thing);

        // The timer goes only if NO integrated Thing remains, all classes included.
        // Releasing it when the last meter of ONE class leaves would switch off the
        // integration of the others - a meter stopping because a neighbour was removed.
        bool resteQuelquun = false;
        const QVector<RegleIntegration> regles = reglesIntegration();
        for (const RegleIntegration &r : regles)
            if (!myThings().filterByThingClassId(r.classe).isEmpty())
                resteQuelquun = true;
        if (m_energieTimer && !resteQuelquun) {
            hardwareManager()->pluginTimerManager()->unregisterTimer(m_energieTimer);
            m_energieTimer = nullptr;
        }
    }
}
