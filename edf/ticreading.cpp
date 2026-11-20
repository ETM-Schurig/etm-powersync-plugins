// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#include "ticreading.h"

namespace {

// Digits only, as the historic mode sends them (zero padded). Anything else is not a number.
std::optional<qint64> number(const QByteArray &data)
{
    if (data.isEmpty() || data.size() > 12)
        return std::nullopt;
    qint64 value = 0;
    for (char c : data) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::optional<int> smallNumber(const QByteArray &data)
{
    const std::optional<qint64> value = number(data);
    if (!value || *value > 1000000)
        return std::nullopt;
    return static_cast<int>(*value);
}

}

const QList<QByteArray> &TicReading::indexLabels()
{
    static const QList<QByteArray> labels {
        "BASE", "HCHC", "HCHP", "EJPHN", "EJPHPM",
        "BBRHCJB", "BBRHPJB", "BBRHCJW", "BBRHPJW", "BBRHCJR", "BBRHPJR"
    };
    return labels;
}

TicReading TicReading::fromFrame(const TicDecoder::Frame &frame)
{
    TicReading reading;
    for (const TicDecoder::Group &group : frame) {
        const QByteArray &label = group.first;
        const QByteArray &data = group.second;
        reading.m_labels++;

        if (label == "ADCO") {
            // Personal data: noted as present, not kept.
            reading.m_hasMeterNumber = !data.isEmpty();
        } else if (label == "OPTARIF") {
            reading.m_rawOption = data;
            reading.m_option = optionFromRaw(data);
        } else if (label == "PTEC") {
            reading.m_rawPeriod = data;
            reading.m_period = periodFromRaw(data);
        } else if (label == "ISOUSC") {
            reading.m_isousc = smallNumber(data);
        } else if (label == "PAPP") {
            reading.m_papp = smallNumber(data);
        } else if (label == "IINST") {
            reading.m_iinst[0] = smallNumber(data);
        } else if (label == "IINST1" || label == "IINST2" || label == "IINST3") {
            reading.m_iinst[label.at(5) - '0'] = smallNumber(data);
            reading.m_threePhase = true;
        } else if (label == "ADPS" || label == "ADIR1" || label == "ADIR2" || label == "ADIR3") {
            reading.m_adps = true;
            if (label.startsWith("ADIR"))
                reading.m_threePhase = true;
        } else if (label == "IMAX1" || label == "IMAX2" || label == "IMAX3" || label == "PMAX" || label == "PPOT") {
            reading.m_threePhase = true;
        } else if (label == "PEJP") {
            reading.m_pejp = smallNumber(data);
            if (!reading.m_pejp)
                reading.m_pejp = 30; // present but unreadable: the notice is the presence itself
        } else if (label == "DEMAIN") {
            if (data == "BLEU")
                reading.m_demain = TempoColor::Blue;
            else if (data == "BLAN")
                reading.m_demain = TempoColor::White;
            else if (data == "ROUG")
                reading.m_demain = TempoColor::Red;
        } else if (indexLabels().contains(label)) {
            const std::optional<qint64> wh = number(data);
            if (wh)
                reading.m_indexes.insert(label, *wh);
        }
    }
    return reading;
}

std::optional<int> TicReading::currentA(int phase) const
{
    if (phase < 1 || phase > 3)
        return std::nullopt;
    if (m_threePhase)
        return m_iinst[phase];
    return phase == 1 ? m_iinst[0] : std::nullopt;
}

std::optional<qint64> TicReading::indexWh(const QByteArray &label) const
{
    if (!m_indexes.contains(label))
        return std::nullopt;
    return m_indexes.value(label);
}

TicReading::Option TicReading::optionFromRaw(const QByteArray &optarif)
{
    if (optarif == "BASE")
        return Option::Base;
    if (optarif.startsWith("HC"))
        return Option::HcHp;   // "HC.."
    if (optarif.startsWith("EJP"))
        return Option::Ejp;    // "EJP."
    if (optarif.startsWith("BBR"))
        return Option::Tempo;  // "BBRx", x = the program of the meter's switching contact
    return Option::Unknown;
}

TicReading::Period TicReading::periodFromRaw(const QByteArray &ptec)
{
    static const QHash<QByteArray, Period> periods {
        { "TH..", Period::AllHours },
        { "HC..", Period::OffPeak },
        { "HP..", Period::Peak },
        { "HN..", Period::EjpNormal },
        { "PM..", Period::EjpMobilePeak },
        { "HCJB", Period::TempoBlueOffPeak },
        { "HPJB", Period::TempoBluePeak },
        { "HCJW", Period::TempoWhiteOffPeak },
        { "HPJW", Period::TempoWhitePeak },
        { "HCJR", Period::TempoRedOffPeak },
        { "HPJR", Period::TempoRedPeak },
    };
    return periods.value(ptec, Period::Unknown);
}

QString TicReading::optionCode(Option option)
{
    switch (option) {
    case Option::Base: return QStringLiteral("base");
    case Option::HcHp: return QStringLiteral("hchp");
    case Option::Ejp: return QStringLiteral("ejp");
    case Option::Tempo: return QStringLiteral("tempo");
    case Option::Unknown: break;
    }
    return QStringLiteral("unknown");
}

QString TicReading::periodCode(Period period)
{
    switch (period) {
    case Period::AllHours: return QStringLiteral("base");
    case Period::OffPeak: return QStringLiteral("hc");
    case Period::Peak: return QStringLiteral("hp");
    case Period::EjpNormal: return QStringLiteral("hn");
    case Period::EjpMobilePeak: return QStringLiteral("pm");
    case Period::TempoBlueOffPeak: return QStringLiteral("hcjb");
    case Period::TempoBluePeak: return QStringLiteral("hpjb");
    case Period::TempoWhiteOffPeak: return QStringLiteral("hcjw");
    case Period::TempoWhitePeak: return QStringLiteral("hpjw");
    case Period::TempoRedOffPeak: return QStringLiteral("hcjr");
    case Period::TempoRedPeak: return QStringLiteral("hpjr");
    case Period::Unknown: break;
    }
    return QStringLiteral("unknown");
}

QString TicReading::periodLabel(Period period)
{
    switch (period) {
    case Period::AllHours: return QStringLiteral("Toutes heures");
    case Period::OffPeak: return QStringLiteral("Heures creuses");
    case Period::Peak: return QStringLiteral("Heures pleines");
    case Period::EjpNormal: return QStringLiteral("Heures normales");
    case Period::EjpMobilePeak: return QStringLiteral("Pointe mobile");
    case Period::TempoBlueOffPeak: return QStringLiteral("Heures creuses jour bleu");
    case Period::TempoBluePeak: return QStringLiteral("Heures pleines jour bleu");
    case Period::TempoWhiteOffPeak: return QStringLiteral("Heures creuses jour blanc");
    case Period::TempoWhitePeak: return QStringLiteral("Heures pleines jour blanc");
    case Period::TempoRedOffPeak: return QStringLiteral("Heures creuses jour rouge");
    case Period::TempoRedPeak: return QStringLiteral("Heures pleines jour rouge");
    case Period::Unknown: break;
    }
    return QStringLiteral("Inconnue");
}
