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

#include "integrationpluginedf.h"
#include "plugininfo.h"
#include "ejpdeclaration.h"
#include "priceschedule.h"
#include "regulatedtariff.h"

#include <integrations/thing.h>
#include <integrations/thingactioninfo.h>
#include <integrations/thingdescriptor.h>
#include <integrations/thingdiscoveryinfo.h>
#include <integrations/thingsetupinfo.h>

#include <QSettings>
#include <QTimeZone>

namespace {

// A reading older than this is not used to follow the contract (the port went quiet).
constexpr int TicFreshS = 30;
constexpr int TickMs = 60 * 1000;

QString periodLabel(const QString &code)
{
    static const QHash<QString, QString> labels {
        { QStringLiteral("base"), QStringLiteral("Toutes heures") },
        { QStringLiteral("hc"), QStringLiteral("Heures creuses") },
        { QStringLiteral("hp"), QStringLiteral("Heures pleines") },
        { QStringLiteral("hn"), QStringLiteral("Heures normales") },
        { QStringLiteral("pm"), QStringLiteral("Pointe mobile") },
    };
    return labels.value(code, QStringLiteral("Inconnue"));
}

QString optionLabel(TicReading::Option option)
{
    switch (option) {
    case TicReading::Option::Base: return QStringLiteral("Base");
    case TicReading::Option::HcHp: return QStringLiteral("Heures creuses");
    case TicReading::Option::Ejp: return QStringLiteral("EJP");
    case TicReading::Option::Tempo: return QStringLiteral("Tempo");
    case TicReading::Option::Unknown: break;
    }
    return QStringLiteral("inconnue");
}

QString typeLabel(Contract::Type type)
{
    switch (type) {
    case Contract::Type::Base: return QStringLiteral("Base");
    case Contract::Type::HcHp: return QStringLiteral("Heures creuses");
    case Contract::Type::Ejp: return QStringLiteral("EJP");
    case Contract::Type::Tempo: return QStringLiteral("Tempo");
    }
    return QString();
}

QString typeParamValue(Contract::Type type)
{
    switch (type) {
    case Contract::Type::Base: return QStringLiteral("Base");
    case Contract::Type::HcHp: return QStringLiteral("Heures creuses / heures pleines");
    case Contract::Type::Ejp: return QStringLiteral("EJP");
    case Contract::Type::Tempo: return QStringLiteral("Tempo (pas encore pris en charge)");
    }
    return QString();
}

QString tempoColor(TicReading::TempoColor color)
{
    switch (color) {
    case TicReading::TempoColor::Blue: return QStringLiteral("bleu");
    case TicReading::TempoColor::White: return QStringLiteral("blanc");
    case TicReading::TempoColor::Red: return QStringLiteral("rouge");
    case TicReading::TempoColor::Unknown: break;
    }
    return QStringLiteral("inconnue");
}

QDateTime localNow()
{
    return QDateTime::currentDateTime().toTimeZone(QTimeZone::systemTimeZone());
}

}

IntegrationPluginEdf::IntegrationPluginEdf()
{
}

std::optional<Contract::Type> IntegrationPluginEdf::typeFromParam(const QString &value)
{
    for (Contract::Type type : { Contract::Type::Base, Contract::Type::HcHp, Contract::Type::Ejp, Contract::Type::Tempo }) {
        if (typeParamValue(type) == value)
            return type;
    }
    return std::nullopt;
}

Contract::Type IntegrationPluginEdf::typeFromOption(TicReading::Option option, bool *known)
{
    *known = true;
    switch (option) {
    case TicReading::Option::Base: return Contract::Type::Base;
    case TicReading::Option::HcHp: return Contract::Type::HcHp;
    case TicReading::Option::Ejp: return Contract::Type::Ejp;
    case TicReading::Option::Tempo: return Contract::Type::Tempo;
    case TicReading::Option::Unknown: break;
    }
    *known = false;
    return Contract::Type::Base;
}

void IntegrationPluginEdf::discoverThings(ThingDiscoveryInfo *info)
{
    if (info->thingClassId() != contractThingClassId) {
        info->finish(Thing::ThingErrorThingClassNotFound);
        return;
    }
    // The contract is offered for every Linky set up: the link is the parameter linkyThing,
    // not a parent/child relation (removing the meter must not remove the contract, and a
    // contract without meter is legitimate).
    for (Thing *linky : myThings().filterByThingClassId(linkyThingClassId)) {
        const TicReading reading = m_lastReading.value(linky);
        bool known = false;
        const Contract::Type type = typeFromOption(reading.option(), &known);
        const QString description = known ? QStringLiteral("Option lue sur le compteur : %1").arg(optionLabel(reading.option()))
                                          : QStringLiteral("Option du compteur pas encore lue");
        ThingDescriptor descriptor(contractThingClassId, QStringLiteral("Contrat d'électricité"), description);
        ParamList params;
        params.append(Param(contractThingLinkyThingParamTypeId, linky->id().toString()));
        if (known)
            params.append(Param(contractThingContractTypeParamTypeId, typeParamValue(type)));
        descriptor.setParams(params);
        for (Thing *existing : myThings().filterByThingClassId(contractThingClassId)) {
            if (existing->paramValue(contractThingLinkyThingParamTypeId).toString() == linky->id().toString())
                descriptor.setThingId(existing->id());
        }
        info->addThingDescriptor(descriptor);
    }
    if (info->thingDescriptors().isEmpty()) {
        info->finish(Thing::ThingErrorNoError,
                     QStringLiteral("Aucun compteur Linky : ajoutez d'abord le compteur, ou le contrat à la main."));
        return;
    }
    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginEdf::setupThing(ThingSetupInfo *info)
{
    if (info->thing()->thingClassId() == linkyThingClassId) {
        setupLinky(info);
    } else if (info->thing()->thingClassId() == contractThingClassId) {
        setupContract(info);
    } else {
        info->finish(Thing::ThingErrorThingClassNotFound);
    }
}

// ---------------------------------------------------------------------------- Linky

void IntegrationPluginEdf::setupLinky(ThingSetupInfo *info)
{
    Thing *thing = info->thing();
    const QString portName = thing->paramValue(linkyThingSerialPortParamTypeId).toString().trimmed();
    if (portName.isEmpty()) {
        info->finish(Thing::ThingErrorInvalidParameter, QStringLiteral("Saisissez le port série de la TIC (par exemple /dev/serial0)."));
        return;
    }
    if (m_ports.contains(thing))
        delete m_ports.take(thing);
    m_lastReading.remove(thing);
    m_lastReadingAt.remove(thing);

    TicPort *port = new TicPort(this);
    port->setPortName(portName);
    port->setMode(thing->paramValue(linkyThingSerialModeParamTypeId).toString().startsWith(QStringLiteral("7E1"))
                  ? TicPort::Mode::SevenEven : TicPort::Mode::EightNoneMasked);
    m_ports.insert(thing, port);

    connect(port, &TicPort::frameReceived, thing, [this, thing](const TicReading &reading) {
        onTicFrame(thing, reading);
    });
    connect(port, &TicPort::connectedChanged, thing, [this, thing, port](bool connected) {
        thing->setStateValue(linkyConnectedStateTypeId, connected);
        if (connected) {
            qCInfo(dcEdf()) << thing->name() << "receives TIC frames on" << port->portName();
        } else {
            qCWarning(dcEdf()) << thing->name() << "receives no valid TIC frame on" << port->portName()
                               << (port->portOpen() ? "(port open, silent)" : "(port not open)");
            thing->setStateValue(linkyTicModeStateTypeId, port->looksLikeStandardMode()
                                 ? QStringLiteral("standard (non pris en charge)") : QStringLiteral("aucune trame"));
            thing->setStateValue(linkyEjpNoticeStateTypeId, false);
            thing->setStateValue(linkyEjpNoticeMinutesStateTypeId, 0);
        }
        for (Thing *contract : myThings().filterByThingClassId(contractThingClassId)) {
            if (linkedLinky(contract) == thing)
                updateContract(contract);
        }
    });
    connect(port, &TicPort::portError, thing, [thing](const QString &error) {
        qCWarning(dcEdf()) << thing->name() << error;
    });

    thing->setStateValue(linkyConnectedStateTypeId, false);
    port->start();
    // A port absent at setup (adapter not plugged yet, simulator not started) is not an
    // error: the thing is set up disconnected and the port is retried.
    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginEdf::onTicFrame(Thing *linky, const TicReading &reading)
{
    const TicReading previous = m_lastReading.value(linky);
    const bool hadReading = m_lastReading.contains(linky);
    m_lastReading.insert(linky, reading);
    m_lastReadingAt.insert(linky, QDateTime::currentDateTimeUtc());
    updateLinkyStates(linky, reading);

    if (!hadReading) {
        // Never the meter number (ADCO): personal data.
        qCInfo(dcEdf()) << linky->name() << "historic TIC: option" << reading.rawOption()
                        << "period" << reading.rawPeriod() << (reading.threePhase() ? "three phase" : "single phase");
    }
    // The contract follows the meter on what changes it, not on every frame (~1.5 s).
    if (hadReading && previous.option() == reading.option() && previous.period() == reading.period()
            && previous.ejpNotice() == reading.ejpNotice())
        return;
    for (Thing *contract : myThings().filterByThingClassId(contractThingClassId)) {
        if (linkedLinky(contract) == linky)
            updateContract(contract);
    }
}

void IntegrationPluginEdf::updateLinkyStates(Thing *thing, const TicReading &reading)
{
    thing->setStateValue(linkyTicModeStateTypeId, QStringLiteral("historique"));
    if (reading.option() != TicReading::Option::Unknown)
        thing->setStateValue(linkyContractOptionStateTypeId, TicReading::optionCode(reading.option()));
    thing->setStateValue(linkyCurrentPeriodStateTypeId, TicReading::periodCode(reading.period()));
    thing->setStateValue(linkyCurrentPeriodLabelStateTypeId, TicReading::periodLabel(reading.period()));
    thing->setStateValue(linkyEjpNoticeStateTypeId, reading.ejpNotice());
    thing->setStateValue(linkyEjpNoticeMinutesStateTypeId, reading.ejpNoticeMinutes());
    if (reading.option() == TicReading::Option::Tempo)
        thing->setStateValue(linkyTempoTomorrowStateTypeId, tempoColor(reading.tempoTomorrow()));

    static const QList<QPair<QByteArray, StateTypeId>> indexes {
        { "BASE", linkyIndexBaseStateTypeId },
        { "HCHC", linkyIndexHcStateTypeId },
        { "HCHP", linkyIndexHpStateTypeId },
        { "EJPHN", linkyIndexEjpHnStateTypeId },
        { "EJPHPM", linkyIndexEjpPmStateTypeId },
        { "BBRHCJB", linkyIndexTempoHcBlueStateTypeId },
        { "BBRHPJB", linkyIndexTempoHpBlueStateTypeId },
        { "BBRHCJW", linkyIndexTempoHcWhiteStateTypeId },
        { "BBRHPJW", linkyIndexTempoHpWhiteStateTypeId },
        { "BBRHCJR", linkyIndexTempoHcRedStateTypeId },
        { "BBRHPJR", linkyIndexTempoHpRedStateTypeId },
    };
    for (const auto &index : indexes) {
        const std::optional<qint64> wh = reading.indexWh(index.first);
        if (wh)
            thing->setStateValue(index.second, *wh / 1000.0);
    }

    thing->setStateValue(linkyThreePhaseStateTypeId, reading.threePhase());
    if (reading.currentA(1))
        thing->setStateValue(linkyCurrentPhaseAStateTypeId, *reading.currentA(1));
    if (reading.threePhase()) {
        if (reading.currentA(2))
            thing->setStateValue(linkyCurrentPhaseBStateTypeId, *reading.currentA(2));
        if (reading.currentA(3))
            thing->setStateValue(linkyCurrentPhaseCStateTypeId, *reading.currentA(3));
    }
    if (reading.apparentPowerVA())
        thing->setStateValue(linkyApparentPowerStateTypeId, *reading.apparentPowerVA());
    if (reading.subscribedCurrentA())
        thing->setStateValue(linkySubscribedCurrentStateTypeId, *reading.subscribedCurrentA());
    thing->setStateValue(linkyOvercurrentWarningStateTypeId, reading.overcurrentWarning());

    if (TicPort *port = m_ports.value(thing)) {
        thing->setStateValue(linkyBadChecksumsStateTypeId, port->decoder().badChecksums());
        thing->setStateValue(linkyInterruptedFramesStateTypeId, port->decoder().interruptedFrames());
    }
}

// ---------------------------------------------------------------------------- Contract

void IntegrationPluginEdf::setupContract(ThingSetupInfo *info)
{
    Thing *thing = info->thing();
    // Set up again after a reconfiguration: the old contract stops first.
    thingRemoved(thing);

    const std::optional<Contract::Type> type = typeFromParam(thing->paramValue(contractThingContractTypeParamTypeId).toString());
    if (!type) {
        info->finish(Thing::ThingErrorInvalidParameter, QStringLiteral("Choisissez l'option du contrat."));
        return;
    }

    Contract::Settings settings;
    settings.type = *type;
    settings.priceSource = thing->paramValue(contractThingPriceSourceParamTypeId).toString().startsWith(QStringLiteral("Prix saisis"))
            ? Contract::PriceSource::Manual : Contract::PriceSource::Regulated;
    settings.subscribedKva = thing->paramValue(contractThingSubscribedPowerParamTypeId).toInt();
    settings.offPeakRanges = thing->paramValue(contractThingOffPeakRangesParamTypeId).toString();
    settings.manualPrices = {
        { QStringLiteral("base"), thing->paramValue(contractThingPriceBaseParamTypeId).toDouble() },
        { QStringLiteral("hc"), thing->paramValue(contractThingPriceHcParamTypeId).toDouble() },
        { QStringLiteral("hp"), thing->paramValue(contractThingPriceHpParamTypeId).toDouble() },
        { QStringLiteral("hn"), thing->paramValue(contractThingPriceEjpHnParamTypeId).toDouble() },
        { QStringLiteral("pm"), thing->paramValue(contractThingPriceEjpPmParamTypeId).toDouble() },
    };

    const RegulatedTariff &tariff = RegulatedTariff::builtIn();
    if (settings.priceSource == Contract::PriceSource::Regulated && !tariff.isValid()) {
        info->finish(Thing::ThingErrorSetupFailed, QStringLiteral("Le barème réglementé fourni est illisible : choisissez « prix saisis »."));
        return;
    }
    QString error;
    const std::optional<Contract> contract = Contract::create(settings, tariff, &error);
    if (!contract) {
        qCWarning(dcEdf()) << thing->name() << "refused:" << error;
        info->finish(Thing::ThingErrorInvalidParameter, error);
        return;
    }

    // A linked meter that already said its option must agree with the contract.
    if (Thing *linky = linkedLinky(thing)) {
        bool known = false;
        const Contract::Type read = typeFromOption(m_lastReading.value(linky).option(), &known);
        if (known && read != *type) {
            const QString message = QStringLiteral("Le compteur Linky lié lit l'option %1, le contrat choisi est %2 : choisissez l'option lue sur le compteur.")
                    .arg(typeLabel(read), typeLabel(*type));
            qCWarning(dcEdf()) << thing->name() << "refused:" << message;
            info->finish(Thing::ThingErrorInvalidParameter, message);
            return;
        }
    }

    ContractRuntime runtime;
    runtime.contract = *contract;
    QSettings *storage = pluginStorage();
    storage->beginGroup(thing->id().toString());
    runtime.calendar = EjpCalendar::fromVariant(storage->value(QStringLiteral("ejpCalendar")).toMap());
    // The box counts the EJP days from the day of the next 07:00 after the first setup as an
    // EJP contract; the days before are the number entered. Kept once: a reconfiguration does
    // not move it (a contract set up as Base, then changed to EJP, counts from the change).
    QDate countFrom = QDate::fromString(storage->value(QStringLiteral("ejpCountFrom")).toString(), Qt::ISODate);
    if (!countFrom.isValid() && *type == Contract::Type::Ejp) {
        countFrom = EjpCalendar::declarationTarget(localNow());
        storage->setValue(QStringLiteral("ejpCountFrom"), countFrom.toString(Qt::ISODate));
    }
    storage->endGroup();
    runtime.calendar.setCountStart(countFrom, thing->paramValue(contractThingEjpPeakDaysBeforeParamTypeId).toInt());
    runtime.calendar.prune(localNow().date());

    runtime.tick = new QTimer(thing);
    runtime.tick->start(TickMs);
    connect(runtime.tick, &QTimer::timeout, thing, [this, thing]() { updateContract(thing); });
    runtime.switchTimer = new QTimer(thing);
    runtime.switchTimer->setSingleShot(true);
    connect(runtime.switchTimer, &QTimer::timeout, thing, [this, thing]() { updateContract(thing); });
    m_contracts.insert(thing, runtime);

    if (settings.priceSource == Contract::PriceSource::Regulated && tariff.toVerify())
        qCInfo(dcEdf()) << thing->name() << "uses the regulated tariff of" << tariff.effectiveDate().toString(Qt::ISODate)
                        << "(marked: to verify against the official grid)";

    info->finish(Thing::ThingErrorNoError);

    updateContract(thing);
}

Thing *IntegrationPluginEdf::linkedLinky(Thing *contract) const
{
    const QString id = contract->paramValue(contractThingLinkyThingParamTypeId).toString().trimmed();
    if (id.isEmpty())
        return nullptr;
    const ThingId thingId(id);
    for (Thing *thing : myThings().filterByThingClassId(linkyThingClassId)) {
        if (thing->id() == thingId)
            return thing;
    }
    return nullptr;
}

QString IntegrationPluginEdf::linkStatus(Thing *contract, const ContractRuntime &runtime, QString *ticPeriod) const
{
    ticPeriod->clear();
    if (contract->paramValue(contractThingLinkyThingParamTypeId).toString().trimmed().isEmpty())
        return QStringLiteral("aucun");
    Thing *linky = linkedLinky(contract);
    if (!linky)
        return QStringLiteral("compteur introuvable");
    const TicPort *port = m_ports.value(linky);
    const QDateTime at = m_lastReadingAt.value(linky);
    if (!port || !port->connected() || !at.isValid() || at.secsTo(QDateTime::currentDateTimeUtc()) > TicFreshS)
        return QStringLiteral("lié, pas de trame récente");
    const TicReading reading = m_lastReading.value(linky);
    bool known = false;
    const Contract::Type read = typeFromOption(reading.option(), &known);
    if (!known)
        return QStringLiteral("lié, option non lue");
    if (read != runtime.contract.type())
        return QStringLiteral("option différente : compteur %1, contrat %2 (TIC ignorée)")
                .arg(typeLabel(read), typeLabel(runtime.contract.type()));
    const QString period = TicReading::periodCode(reading.period());
    if (runtime.contract.hasPeriod(period))
        *ticPeriod = period;
    return QStringLiteral("lié");
}

void IntegrationPluginEdf::updateContract(Thing *thing)
{
    if (!m_contracts.contains(thing))
        return;
    ContractRuntime &runtime = m_contracts[thing];
    const Contract &contract = runtime.contract;
    const QTimeZone zone = QTimeZone::systemTimeZone();
    const QDateTime now = localNow();
    const QDate today = now.date();

    QString ticPeriod;
    const QString link = linkStatus(thing, runtime, &ticPeriod);
    if (link != runtime.lastLinkyStatus) {
        qCInfo(dcEdf()) << thing->name() << "Linky:" << link;
        runtime.lastLinkyStatus = link;
    }
    thing->setStateValue(contractLinkyStatusStateTypeId, link);

    if (contract.type() == Contract::Type::Ejp && link == QStringLiteral("lié")) {
        const TicReading reading = m_lastReading.value(linkedLinky(thing));
        if (runtime.calendar.observeTic(now, reading.period(), reading.ejpNotice())) {
            qCInfo(dcEdf()) << thing->name() << "EJP from the TIC: day" << EjpCalendar::ejpDayOf(now).toString(Qt::ISODate)
                            << EjpCalendar::statusCode(runtime.calendar.ticStatus(EjpCalendar::ejpDayOf(now)));
            saveCalendar(thing);
        }
    }
    runtime.calendar.prune(today);

    const PriceSchedule schedule = PriceSchedule::build(contract, runtime.calendar, now, zone, ticPeriod);
    const PriceSchedule::Slot &current = schedule.current();
    thing->setStateValue(contractCurrentPeriodStateTypeId, current.period);
    thing->setStateValue(contractCurrentPeriodLabelStateTypeId, periodLabel(current.period));
    thing->setStateValue(contractPriceKnownStateTypeId, current.price.has_value());
    // Every slot has a price (an EJP day neither read nor declared is presumed normal); the
    // dearest period stays as a guard should a slot ever come without one.
    const QString dearest = contract.periods().last();
    thing->setStateValue(contractCurrentMarketPriceStateTypeId, current.price.value_or(contract.price(dearest)));
    thing->setStateValue(contractRankStateTypeId, current.nominal.value_or(100));
    thing->setStateValue(contractCurrentSourceStateTypeId, current.source);
    const QDateTime next = schedule.nextSwitch();
    thing->setStateValue(contractNextSwitchStateTypeId, next.isValid() ? next.toString(Qt::ISODate) : QString());
    thing->setStateValue(contractValidUntilStateTypeId, (next.isValid() ? next : schedule.horizonEnd()).toSecsSinceEpoch());

    const RegulatedTariff &tariff = RegulatedTariff::builtIn();
    if (contract.priceSource() == Contract::PriceSource::Regulated) {
        thing->setStateValue(contractTariffSourceStateTypeId, QStringLiteral("%1, en vigueur au %2%3")
                             .arg(tariff.name(), tariff.effectiveDate().toString(Qt::ISODate),
                                  tariff.toVerify() ? QStringLiteral(" (à vérifier)") : QString()));
    } else {
        thing->setStateValue(contractTariffSourceStateTypeId, QStringLiteral("Prix saisis"));
    }
    thing->setStateValue(contractRegulatedTariffDateStateTypeId, tariff.effectiveDate().toString(Qt::ISODate));
    thing->setStateValue(contractRegulatedTariffToVerifyStateTypeId, tariff.toVerify());

    QString discrepancy;
    if (contract.type() == Contract::Type::Ejp) {
        const EjpCalendar::Day todayInfo = runtime.calendar.dayOrPresumed(EjpCalendar::ejpDayOf(now));
        const EjpCalendar::Day tomorrowInfo = runtime.calendar.dayOrPresumed(EjpCalendar::ejpDayOf(now).addDays(1));
        thing->setStateValue(contractEjpTodayStateTypeId, EjpCalendar::statusCode(todayInfo.status));
        thing->setStateValue(contractEjpTodaySourceStateTypeId, EjpCalendar::sourceCode(todayInfo.source));
        thing->setStateValue(contractEjpTomorrowStateTypeId, EjpCalendar::statusCode(tomorrowInfo.status));
        thing->setStateValue(contractEjpTomorrowSourceStateTypeId, EjpCalendar::sourceCode(tomorrowInfo.source));
        // The counter runs on the EJP day in progress, not the calendar date: a day counts from
        // the start of its peak (07:00), never while it is only declared or announced.
        const QDate counterDay = EjpCalendar::ejpDayOf(now);
        thing->setStateValue(contractEjpSeasonPeakDaysStateTypeId, runtime.calendar.seasonPeakDays(counterDay));
        thing->setStateValue(contractEjpSeasonUnknownDaysStateTypeId, runtime.calendar.seasonUnknownDays(counterDay));
        thing->setStateValue(contractEjpSeasonCompleteStateTypeId, runtime.calendar.seasonCountComplete(counterDay));
        thing->setStateValue(contractEjpSeasonCountStartStateTypeId, runtime.calendar.seasonCountStart(counterDay).toString(Qt::ISODate));
        discrepancy = runtime.calendar.discrepancy(EjpCalendar::ejpDayOf(now));
    } else if (contract.type() == Contract::Type::HcHp && !ticPeriod.isEmpty()) {
        const QString fromRanges = contract.hcHpPeriodAt(now.time());
        if (fromRanges != ticPeriod) {
            discrepancy = QStringLiteral("%1 : TIC %2, plages saisies %3")
                    .arg(now.toString(QStringLiteral("HH:mm")), periodLabel(ticPeriod).toLower(), periodLabel(fromRanges).toLower());
        }
    }
    if (discrepancy != thing->stateValue(contractDiscrepancyStateTypeId).toString() && !discrepancy.isEmpty())
        qCWarning(dcEdf()) << thing->name() << "discrepancy:" << discrepancy;
    thing->setStateValue(contractDiscrepancyStateTypeId, discrepancy);

    // ISOUSC of the linked meter against the power entered: published, never corrected.
    QString powerDiscrepancy;
    if (Thing *linky = linkedLinky(thing)) {
        const TicReading reading = m_lastReading.value(linky);
        if (reading.subscribedCurrentA())
            powerDiscrepancy = Contract::subscribedPowerDiscrepancy(contract.subscribedKva(), *reading.subscribedCurrentA(), reading.threePhase());
    }
    if (powerDiscrepancy != thing->stateValue(contractSubscribedPowerDiscrepancyStateTypeId).toString() && !powerDiscrepancy.isEmpty())
        qCWarning(dcEdf()) << thing->name() << "subscribed power:" << powerDiscrepancy;
    thing->setStateValue(contractSubscribedPowerDiscrepancyStateTypeId, powerDiscrepancy);

    // The series: republished only when its content changes (not every minute for generatedAt).
    QVariantMap transport = schedule.toVariant(contract);
    QVariantMap content = transport;
    content.remove(QStringLiteral("generatedAt"));
    if (content != runtime.lastTransport) {
        runtime.lastTransport = content;
        thing->setStateValue(contractPriceSlotsTransportStateTypeId, transport);
    }

    if (next.isValid())
        runtime.switchTimer->start(static_cast<int>(qBound<qint64>(qint64(1000), now.msecsTo(next) + 1000, qint64(TickMs) * 60)));
}

void IntegrationPluginEdf::executeAction(ThingActionInfo *info)
{
    Thing *thing = info->thing();
    const ActionTypeId actionTypeId = info->action().actionTypeId();
    if (thing->thingClassId() != contractThingClassId || !m_contracts.contains(thing)
            || (actionTypeId != contractDeclareEjpTomorrowActionTypeId && actionTypeId != contractCancelEjpTomorrowActionTypeId)) {
        info->finish(Thing::ThingErrorActionTypeNotFound);
        return;
    }
    ContractRuntime &runtime = m_contracts[thing];
    const bool declare = actionTypeId == contractDeclareEjpTomorrowActionTypeId;
    const EjpDeclaration::Outcome outcome = declare
            ? EjpDeclaration::declareTomorrow(runtime.contract.type(), runtime.calendar, localNow())
            : EjpDeclaration::cancelTomorrow(runtime.contract.type(), runtime.calendar, localNow());
    qCInfo(dcEdf()) << thing->name() << (declare ? "declare EJP day" : "cancel EJP declaration")
                    << outcome.day.toString(Qt::ISODate) << (outcome.accepted ? "accepted" : "refused") << outcome.message;
    if (outcome.changed) {
        saveCalendar(thing);
        updateContract(thing);
    }
    if (!outcome.accepted) {
        info->finish(runtime.contract.type() == Contract::Type::Ejp ? Thing::ThingErrorInvalidParameter
                                                                    : Thing::ThingErrorUnsupportedFeature,
                     outcome.message);
        return;
    }
    info->finish(Thing::ThingErrorNoError, outcome.message);
}

void IntegrationPluginEdf::saveCalendar(Thing *thing)
{
    if (!m_contracts.contains(thing))
        return;
    QSettings *storage = pluginStorage();
    storage->beginGroup(thing->id().toString());
    storage->setValue(QStringLiteral("ejpCalendar"), m_contracts.value(thing).calendar.toVariant());
    storage->endGroup();
}

void IntegrationPluginEdf::thingRemoved(Thing *thing)
{
    if (m_ports.contains(thing)) {
        TicPort *port = m_ports.take(thing);
        port->stop();
        port->deleteLater();
        m_lastReading.remove(thing);
        m_lastReadingAt.remove(thing);
    }
    if (m_contracts.contains(thing)) {
        ContractRuntime runtime = m_contracts.take(thing);
        delete runtime.tick;
        delete runtime.switchTimer;
    }
}
