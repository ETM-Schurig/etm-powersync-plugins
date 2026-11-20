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

#include "integrationplugintrydan.h"
#include "plugininfo.h"
#include "trydandiscovery.h"

#include <hardwaremanager.h>
#include <integrations/thing.h>
#include <integrations/thingactioninfo.h>
#include <integrations/thingdescriptor.h>
#include <integrations/thingdiscoveryinfo.h>
#include <integrations/thingsetupinfo.h>

#include <QHostAddress>
#include <QSettings>

namespace {

// PauseDynamic is retried at most this often when the charger does not take it, so that
// a firmware that refuses it is not written to on every poll.
constexpr int PauseDynamicRetryS = 600;

}

IntegrationPluginTrydan::IntegrationPluginTrydan()
{
}

void IntegrationPluginTrydan::discoverThings(ThingDiscoveryInfo *info)
{
    if (!hardwareManager()->networkDeviceDiscovery()->available()) {
        info->finish(Thing::ThingErrorHardwareNotAvailable,
                     QT_TR_NOOP("The network discovery is not available. Enter the IP address of the charger."));
        return;
    }

    TrydanDiscovery *discovery = new TrydanDiscovery(hardwareManager()->networkDeviceDiscovery(), info);
    connect(discovery, &TrydanDiscovery::finished, info, [this, info, discovery]() {
        for (const TrydanDiscovery::Result &result : discovery->results()) {
            const QString address = result.address.toString();
            QString description = address;
            if (!result.data.firmwareVersion.isEmpty())
                description += QStringLiteral(" - firmware ") + result.data.firmwareVersion;
            ThingDescriptor descriptor(trydanThingClassId, QStringLiteral("V2C Trydan"), description);
            ParamList params;
            params.append(Param(trydanThingAddressParamTypeId, address));
            descriptor.setParams(params);
            if (Thing *existing = myThings().findByParams(params))
                descriptor.setThingId(existing->id());
            info->addThingDescriptor(descriptor);
        }
        info->finish(Thing::ThingErrorNoError);
    });
    discovery->start();
}

void IntegrationPluginTrydan::setupThing(ThingSetupInfo *info)
{
    Thing *thing = info->thing();
    const QString address = thing->paramValue(trydanThingAddressParamTypeId).toString().trimmed();
    if (QHostAddress(address).protocol() != QAbstractSocket::IPv4Protocol) {
        info->finish(Thing::ThingErrorInvalidParameter, QT_TR_NOOP("The IP address of the charger is not valid."));
        return;
    }
    // No default: the charger does not report its wiring, and a wrong one either hides
    // phase switching or announces three phases on a single-phase supply. A charger added
    // before this parameter existed comes here too, and is reconfigured by the installer.
    if (!TrydanModel::wiringFromParam(thing->paramValue(trydanThingWiringParamTypeId).toString())) {
        qCWarning(dcTrydan()) << thing->name() << "has no wiring set: reconfigure it and choose single or three phase.";
        info->finish(Thing::ThingErrorMissingParameter,
                     QT_TR_NOOP("Choose the wiring of the charger: single phase or three phase, as installed."));
        return;
    }

    if (m_clients.contains(thing))
        delete m_clients.take(thing);
    // Set up again after a reconfiguration: the wiring may have changed.
    m_wiringWarned.remove(thing);
    m_wiringCheck.remove(thing);

    TrydanClient *client = new TrydanClient(address, 80, this);
    m_clients.insert(thing, client);

    // Restore the energy tracking BEFORE the first poll, from the state cache (restored by
    // nymea before setupThing) and the plugin storage. chargeEnergy is cached for this.
    TrydanEnergy energy;
    QSettings *storage = pluginStorage();
    storage->beginGroup(thing->id().toString());
    const bool sessionOpen = storage->value(QStringLiteral("sessionOpen"), false).toBool();
    storage->endGroup();
    energy.restore(sessionOpen,
                   thing->stateValue(trydanChargeEnergyStateTypeId).toDouble(),
                   thing->stateValue(trydanSessionEnergyStateTypeId).toDouble(),
                   thing->stateValue(trydanTotalEnergyConsumedStateTypeId).toDouble());
    m_energy.insert(thing, energy);

    // Until the charger has answered, the phase states follow the wiring alone: no phase
    // switching is announced before ChargeMode has been read.
    applyPhaseStates(thing, TrydanRealTimeData());

    connect(client, &TrydanClient::dataReceived, thing, [this, thing](const TrydanRealTimeData &data) {
        updateStates(thing, data);
        manageInternalOptimizer(thing, data);
    });
    connect(client, &TrydanClient::reachableChanged, thing, [this, thing](bool reachable) {
        if (reachable) {
            qCInfo(dcTrydan()) << thing->name() << "is reachable again.";
        } else {
            // Logged on the transition only: one line per outage, not one per failed poll.
            qCWarning(dcTrydan()) << thing->name() << "is not answering: marked as disconnected after"
                                  << TrydanClient::DefaultErrorLimit << "failed exchanges in a row.";
            setDisconnected(thing);
        }
    });
    connect(client, &TrydanClient::exchangeFailed, thing, [thing](const QString &what, const QString &error) {
        qCDebug(dcTrydan()) << thing->name() << what << "failed:" << error;
    });

    // The first read decides only one thing: whether what answers is a Trydan. A charger
    // that does not answer (powered off, Wi-Fi down when nymea starts) is still set up,
    // disconnected, and comes up with the polling.
    TrydanReply *reply = client->refresh();
    connect(reply, &TrydanReply::finished, info, [this, info, thing, client, reply]() {
        if (reply->error() == TrydanReply::NoError && !client->data().looksLikeTrydan()) {
            qCWarning(dcTrydan()) << "The device at" << client->host() << "does not answer like a Trydan.";
            m_clients.remove(thing);
            m_energy.remove(thing);
            client->deleteLater();
            info->finish(Thing::ThingErrorHardwareNotAvailable,
                         QT_TR_NOOP("The device at this address does not answer like a V2C Trydan."));
            return;
        }
        if (reply->error() != TrydanReply::NoError) {
            qCWarning(dcTrydan()) << thing->name() << "did not answer at setup (" << reply->errorString()
                                  << "); set up disconnected, polling continues.";
            setDisconnected(thing);
        }
        client->startPolling();
        info->finish(Thing::ThingErrorNoError);
    });
    connect(info, &ThingSetupInfo::aborted, client, [this, thing, client]() {
        if (m_clients.value(thing) == client) {
            m_clients.remove(thing);
            m_energy.remove(thing);
        }
        client->deleteLater();
    });
}

void IntegrationPluginTrydan::thingRemoved(Thing *thing)
{
    delete m_clients.take(thing);
    m_energy.remove(thing);
    m_lastPauseDynamicWrite.remove(thing);
    m_wiringWarned.remove(thing);
    m_wiringCheck.remove(thing);
    // The persisted session flag has no owner any more: a charger paired again must not
    // resume a session that no longer exists.
    pluginStorage()->remove(thing->id().toString());
}

void IntegrationPluginTrydan::executeAction(ThingActionInfo *info)
{
    Thing *thing = info->thing();
    const Action action = info->action();
    TrydanClient *client = m_clients.value(thing);
    if (!client || !client->reachable() || !client->hasData()) {
        info->finish(Thing::ThingErrorHardwareNotAvailable, QT_TR_NOOP("The charger is not reachable."));
        return;
    }
    const TrydanRealTimeData &data = client->data();

    // The client skips a write when the reading at the moment it is sent already shows the
    // value: the energy manager re-sends unchanged values, and the charger stores every write.

    if (action.actionTypeId() == trydanPowerActionTypeId) {
        const bool enable = action.paramValue(trydanPowerActionPowerParamTypeId).toBool();
        runGesture(info, client->write(QStringLiteral("Paused"), enable ? 0 : 1),
                   QStringLiteral("power %1").arg(enable ? "on" : "off"));
        return;
    }

    if (action.actionTypeId() == trydanMaxChargingCurrentActionTypeId) {
        // Below the minimum of the charger: charging stops (pause), see
        // TrydanModel::currentCommand(). nymea itself refuses a value under the published
        // minimum, so through nymea this is reached only if the minimum of the charger
        // rose between the check and the send.
        const double requested = action.paramValue(trydanMaxChargingCurrentActionMaxChargingCurrentParamTypeId).toDouble();
        runGesture(info, client->setChargingCurrent(requested),
                   QStringLiteral("maxChargingCurrent %1 A").arg(requested));
        return;
    }

    if (action.actionTypeId() == trydanDesiredPhaseCountActionTypeId) {
        const uint phases = action.paramValue(trydanDesiredPhaseCountActionDesiredPhaseCountParamTypeId).toUInt();
        if (phases != 1 && phases != 3) {
            info->finish(Thing::ThingErrorInvalidParameter, QT_TR_NOOP("The phase count must be 1 or 3."));
            return;
        }
        if (wiredPhases(thing) < 3) {
            // Single-phase wiring: frozen at 1, three phases are never announced.
            if (phases == 1) {
                info->finish(Thing::ThingErrorNoError);
            } else {
                info->finish(Thing::ThingErrorInvalidParameter,
                             QT_TR_NOOP("This charger is wired for a single phase."));
            }
            return;
        }
        if (!data.chargeMode) {
            info->finish(Thing::ThingErrorUnsupportedFeature,
                         QT_TR_NOOP("This charger does not report a phase mode: its firmware cannot switch phases."));
            return;
        }
        const int mode = TrydanModel::chargeModeFor(phases);
        if (*data.chargeMode == mode) {
            info->finish(Thing::ThingErrorNoError);
            return;
        }
        // Not prevented: the switching sequence (6 A, pause, switch, resume) is the energy
        // manager's. Said loudly, because what the charger does with a phase change under
        // load is not documented.
        if (TrydanModel::charging(data)) {
            qCWarning(dcTrydan()) << thing->name() << "phase mode changed to" << phases
                                  << "phase(s) while charging at" << TrydanModel::publishedPower(data).watts
                                  << "W: the charger should be paused first.";
        }
        runGesture(info, client->write(QStringLiteral("ChargeMode"), mode),
                   QStringLiteral("desiredPhaseCount %1").arg(phases));
        return;
    }

    if (action.actionTypeId() == trydanRefreshActionTypeId) {
        TrydanReply *reply = client->refresh();
        connect(reply, &TrydanReply::finished, info, [info, reply]() {
            if (reply->error() == TrydanReply::NoError)
                info->finish(Thing::ThingErrorNoError);
            else
                info->finish(Thing::ThingErrorHardwareNotAvailable, QT_TR_NOOP("The charger did not answer."));
        });
        return;
    }

    info->finish(Thing::ThingErrorActionTypeNotFound);
}

void IntegrationPluginTrydan::runGesture(ThingActionInfo *info, TrydanReply *reply, const QString &action)
{
    Thing *thing = info->thing();
    // An action nymea gave up on is not sent late.
    connect(info, &ThingActionInfo::aborted, reply, &TrydanReply::abandon);
    connect(reply, &TrydanReply::finished, info, [info, thing, reply, action]() {
        // The states were updated from the read-back already.
        const QString written = reply->written().isEmpty() ? QStringLiteral("nothing to write") : reply->written();
        switch (reply->error()) {
        case TrydanReply::NoError:
            if (info->action().actionTypeId() == trydanMaxChargingCurrentActionTypeId && written == QLatin1String("Paused=1")) {
                qCInfo(dcTrydan()) << thing->name() << action << "is below the minimum of the charger: charging paused.";
            } else {
                qCDebug(dcTrydan()) << thing->name() << action << "->" << written;
            }
            info->finish(Thing::ThingErrorNoError);
            return;
        case TrydanReply::NetworkError:
        case TrydanReply::Aborted:
            qCWarning(dcTrydan()) << thing->name() << action << "->" << written << "got no answer:" << reply->errorString();
            info->finish(Thing::ThingErrorHardwareNotAvailable, QT_TR_NOOP("The charger did not answer."));
            return;
        case TrydanReply::Rejected:
            qCWarning(dcTrydan()) << thing->name() << action << "->" << written << "refused:" << reply->errorString();
            info->finish(Thing::ThingErrorHardwareFailure, QT_TR_NOOP("The charger refused the command."));
            return;
        case TrydanReply::NotTaken:
            qCWarning(dcTrydan()) << thing->name() << action << "->" << written << "not taken:" << reply->errorString();
            info->finish(Thing::ThingErrorHardwareFailure, QT_TR_NOOP("The charger accepted the command but did not apply it."));
            return;
        case TrydanReply::Unsupported:
            qCWarning(dcTrydan()) << thing->name() << action << "->" << written << ":" << reply->errorString();
            info->finish(Thing::ThingErrorUnsupportedFeature, QT_TR_NOOP("The charger does not support this command."));
            return;
        }
        info->finish(Thing::ThingErrorHardwareFailure);
    });
}

uint IntegrationPluginTrydan::wiredPhases(Thing *thing) const
{
    // Set up only with a wiring chosen; 1 is never reached, and would be the safe side.
    return TrydanModel::wiringFromParam(thing->paramValue(trydanThingWiringParamTypeId).toString()).value_or(1);
}

void IntegrationPluginTrydan::updateStates(Thing *thing, const TrydanRealTimeData &data)
{
    thing->setStateValue(trydanConnectedStateTypeId, true);

    const bool pluggedIn = TrydanModel::pluggedIn(data, thing->stateValue(trydanPluggedInStateTypeId).toBool());
    thing->setStateValue(trydanPluggedInStateTypeId, pluggedIn);
    // charging, currentPower and the phase currents say the same thing on every reading:
    // ChargePower and ChargeState move before or after the measured currents in a
    // transition (ChargeState stays at "charging" after Paused=1), so the power is checked
    // against the currents and replaced by what they give when it contradicts them.
    const TrydanModel::PublishedPower power = TrydanModel::publishedPower(data);
    thing->setStateValue(trydanChargingStateTypeId, TrydanModel::charging(data));
    if (data.paused)
        thing->setStateValue(trydanPowerStateTypeId, *data.paused == 0);
    if (power.source != TrydanModel::PowerSource::None)
        thing->setStateValue(trydanCurrentPowerStateTypeId, power.watts);
    if (power.source == TrydanModel::PowerSource::PhaseMeasurements && data.chargePower) {
        qCDebug(dcTrydan()) << thing->name() << "ChargePower" << *data.chargePower
                            << "W contradicts the phase currents: published" << power.watts
                            << "W from the currents and voltages.";
    }

    // Published range = clamping range of the gesture: a slider value the gesture would
    // change is a command without effect. Set on change only, it is notified to clients.
    const TrydanModel::CurrentRange range = TrydanModel::currentRange(data);
    const State current = thing->state(trydanMaxChargingCurrentStateTypeId);
    if (current.minValue().toInt() != range.min || current.maxValue().toInt() != range.max)
        thing->setStateMinMaxValues(trydanMaxChargingCurrentStateTypeId, range.min, range.max);
    if (data.intensity)
        thing->setStateValue(trydanMaxChargingCurrentStateTypeId, *data.intensity);

    // Same reading as currentPower, so that the energy manager never pairs a power with
    // the currents of another instant.
    const QList<double> currents = TrydanModel::phaseCurrents(data);
    thing->setStateValue(trydanCurrentPhaseAStateTypeId, currents.at(0));
    thing->setStateValue(trydanCurrentPhaseBStateTypeId, currents.at(1));
    thing->setStateValue(trydanCurrentPhaseCStateTypeId, currents.at(2));
    const bool measured = TrydanModel::phaseMeasurementsAvailable(data);
    thing->setStateValue(trydanPhaseMeasurementsAvailableStateTypeId, measured);
    thing->setStateValue(trydanVoltagePhaseAStateTypeId, measured ? *data.voltageMeasure[0] : 0.0);
    thing->setStateValue(trydanVoltagePhaseBStateTypeId, measured ? *data.voltageMeasure[1] : 0.0);
    thing->setStateValue(trydanVoltagePhaseCStateTypeId, measured ? *data.voltageMeasure[2] : 0.0);

    applyPhaseStates(thing, data);

    if (data.locked)
        thing->setStateValue(trydanLockedStateTypeId, *data.locked != 0);
    if (data.dynamic)
        thing->setStateValue(trydanInternalOptimizerActiveStateTypeId, *data.dynamic == 1);
    if (data.pauseDynamic)
        thing->setStateValue(trydanOptimizerSuspendedStateTypeId, *data.pauseDynamic == 1);
    if (data.slaveError)
        thing->setStateValue(trydanSlaveErrorStateTypeId, static_cast<uint>(qMax(0, *data.slaveError)));
    if (!data.firmwareVersion.isEmpty())
        thing->setStateValue(trydanFirmwareVersionStateTypeId, data.firmwareVersion);

    updateEnergy(thing, data, pluggedIn);
}

void IntegrationPluginTrydan::applyPhaseStates(Thing *thing, const TrydanRealTimeData &data)
{
    const uint wiring = wiredPhases(thing);
    const uint phases = TrydanModel::phaseCount(data, wiring);
    thing->setStateValue(trydanPhaseCountStateTypeId, phases);
    thing->setStateValue(trydanChargeModeStateTypeId,
                         TrydanModel::chargeModeName(TrydanModel::chargeMode(data)));

    // Possible values first: the value set next must be one of them.
    const TrydanModel::DesiredPhases desired = TrydanModel::desiredPhases(data, wiring);
    thing->setStatePossibleValues(trydanDesiredPhaseCountStateTypeId, desired.possibleValues);
    if (desired.value)
        thing->setStateValue(trydanDesiredPhaseCountStateTypeId, *desired.value);

    if (!m_wiringWarned.value(thing)) {
        if (phases > wiring) {
            qCWarning(dcTrydan()) << thing->name() << "draws on" << phases << "phases but is set as wired for"
                                  << wiring << ": check the wiring setting.";
            m_wiringWarned.insert(thing, true);
        } else if (wiring < 3 && TrydanModel::chargeMode(data) == TrydanModel::ChargeMode::Three) {
            qCWarning(dcTrydan()) << thing->name() << "is set as wired for a single phase but its phase mode"
                                     " reads three phase: phase switching stays disabled.";
            m_wiringWarned.insert(thing, true);
        }
    }

    // Read only: a mismatch is said, nothing driven changes because of it.
    const TrydanModel::WiringCheck check = TrydanModel::wiringCheck(data, wiring);
    thing->setStateValue(trydanWiringCheckStateTypeId, TrydanModel::wiringCheckName(check));
    const TrydanModel::WiringCheck previous = m_wiringCheck.value(thing, TrydanModel::WiringCheck::NotChecked);
    if (check != previous && check != TrydanModel::WiringCheck::NotChecked) {
        m_wiringCheck.insert(thing, check);
        if (check == TrydanModel::WiringCheck::Consistent) {
            qCInfo(dcTrydan()) << thing->name() << "wiring check: the voltages on L2/L3 match the wiring set.";
        } else {
            qCWarning(dcTrydan()) << thing->name() << "wiring check:" << TrydanModel::wiringCheckName(check)
                                  << "- voltages L1..L3" << *data.voltageMeasure[0] << *data.voltageMeasure[1]
                                  << *data.voltageMeasure[2] << "V. Check the wiring parameter of this charger.";
        }
    }
}

void IntegrationPluginTrydan::updateEnergy(Thing *thing, const TrydanRealTimeData &data, bool pluggedIn)
{
    if (!data.chargeEnergy || !m_energy.contains(thing))
        return;

    TrydanEnergy &energy = m_energy[thing];
    const TrydanEnergy::Result result = energy.update(*data.chargeEnergy, pluggedIn);
    if (result.chargerReset) {
        qCDebug(dcTrydan()) << thing->name() << "ChargeEnergy reset by the charger ("
                            << thing->stateValue(trydanChargeEnergyStateTypeId).toDouble() << "->"
                            << *data.chargeEnergy << "kWh), session at" << result.sessionEnergy << "kWh";
    }
    if (result.sessionClosed) {
        qCDebug(dcTrydan()) << thing->name() << "session closed at" << result.sessionEnergy
                            << "kWh (unplug confirmed on" << TrydanEnergy::AbsencesToClose << "polls)";
        storeSessionOpen(thing, false);
    }
    if (result.sessionOpened) {
        qCDebug(dcTrydan()) << thing->name() << "new session, starting at" << result.sessionEnergy << "kWh";
        storeSessionOpen(thing, true);
    }
    thing->setStateValue(trydanSessionEnergyStateTypeId, result.sessionEnergy);
    thing->setStateValue(trydanTotalEnergyConsumedStateTypeId, result.totalEnergy);
    // Published after the two built values: it is their reference after a restart.
    thing->setStateValue(trydanChargeEnergyStateTypeId, *data.chargeEnergy);
}

void IntegrationPluginTrydan::storeSessionOpen(Thing *thing, bool open)
{
    // On transitions only: the storage is on the flash of the gateway.
    QSettings *storage = pluginStorage();
    storage->beginGroup(thing->id().toString());
    storage->setValue(QStringLiteral("sessionOpen"), open);
    storage->endGroup();
}

void IntegrationPluginTrydan::manageInternalOptimizer(Thing *thing, const TrydanRealTimeData &data)
{
    // The Trydan has its own power control ("Dynamic"). While it runs it overrides the
    // Intensity setpoint, and the maxChargingCurrent gesture would not be taken. It is
    // suspended with PauseDynamic=1, never by writing Dynamic=0: without Dynamic the charger
    // stops reporting its power. Suspended for as long as the setting allows - not toggled
    // with the pause, because the energy manager pauses the charger exactly when it
    // switches phases, and the charger's own control must not act in that window.
    if (!data.dynamic || *data.dynamic != 1)
        return;
    if (!thing->setting(trydanSettingsSuspendInternalOptimizerParamTypeId).toBool())
        return;
    if (data.pauseDynamic && *data.pauseDynamic == 1)
        return;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = m_lastPauseDynamicWrite.value(thing);
    if (last.isValid() && last.secsTo(now) < PauseDynamicRetryS)
        return;
    m_lastPauseDynamicWrite.insert(thing, now);

    TrydanClient *client = m_clients.value(thing);
    if (!client)
        return;
    qCInfo(dcTrydan()) << thing->name() << "own power control is active: suspending it (PauseDynamic=1).";
    TrydanReply *reply = client->write(QStringLiteral("PauseDynamic"), 1);
    connect(reply, &TrydanReply::finished, thing, [thing, reply]() {
        if (reply->error() != TrydanReply::NoError) {
            qCWarning(dcTrydan()) << thing->name() << "could not suspend its own power control:"
                                  << reply->errorString() << "- the setpoint may be overridden.";
        }
    });
}

void IntegrationPluginTrydan::setDisconnected(Thing *thing)
{
    thing->setStateValue(trydanConnectedStateTypeId, false);
    thing->setStateValue(trydanChargingStateTypeId, false);
    thing->setStateValue(trydanCurrentPowerStateTypeId, 0.0);
    thing->setStateValue(trydanCurrentPhaseAStateTypeId, 0.0);
    thing->setStateValue(trydanCurrentPhaseBStateTypeId, 0.0);
    thing->setStateValue(trydanCurrentPhaseCStateTypeId, 0.0);
    // pluggedIn is left as it was: a dead link is not an unplug, and the session tracking
    // only moves on successful polls.
}
