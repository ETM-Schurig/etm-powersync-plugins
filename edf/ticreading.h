// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later.

#ifndef TICREADING_H
#define TICREADING_H

#include "ticdecoder.h"

#include <QHash>
#include <QMetaType>
#include <QString>
#include <optional>

// What a historic-mode frame says, interpreted. Only what the plugin publishes or uses.
//
// The meter number (ADCO) is personal data: it is read only to know that it is present.
// It is never kept here, never logged in clear, never published (see hasMeterNumber()).
class TicReading
{
public:
    // The tariff option of the meter (OPTARIF).
    enum class Option { Unknown, Base, HcHp, Ejp, Tempo };
    // The tariff period in progress (PTEC), in the historic vocabulary.
    enum class Period {
        Unknown,
        AllHours,        // TH..  (Base)
        OffPeak,         // HC..
        Peak,            // HP..
        EjpNormal,       // HN..
        EjpMobilePeak,   // PM..
        TempoBlueOffPeak, TempoBluePeak,    // HCJB, HPJB
        TempoWhiteOffPeak, TempoWhitePeak,  // HCJW, HPJW
        TempoRedOffPeak, TempoRedPeak       // HCJR, HPJR
    };
    enum class TempoColor { Unknown, Blue, White, Red };

    static TicReading fromFrame(const TicDecoder::Frame &frame);

    bool isEmpty() const { return m_labels == 0; }
    int labelCount() const { return m_labels; }

    bool hasMeterNumber() const { return m_hasMeterNumber; }

    Option option() const { return m_option; }
    QByteArray rawOption() const { return m_rawOption; }   // OPTARIF as sent (never personal)
    Period period() const { return m_period; }
    QByteArray rawPeriod() const { return m_rawPeriod; }

    std::optional<int> subscribedCurrentA() const { return m_isousc; }   // ISOUSC
    std::optional<int> apparentPowerVA() const { return m_papp; }        // PAPP, VA (not W)
    // IINST (single phase) or IINST1..3 (three phase), A
    std::optional<int> currentA(int phase) const;
    bool threePhase() const { return m_threePhase; }
    bool overcurrentWarning() const { return m_adps; }                   // ADPS / ADIR1..3

    // EJP notice (PEJP): present 30 min before a mobile peak starts. Value in minutes.
    bool ejpNotice() const { return m_pejp.has_value(); }
    int ejpNoticeMinutes() const { return m_pejp.value_or(0); }

    TempoColor tempoTomorrow() const { return m_demain; }                // DEMAIN

    // Energy indexes, Wh, by historic label: BASE, HCHC, HCHP, EJPHN, EJPHPM, BBRHCJB ...
    const QHash<QByteArray, qint64> &indexesWh() const { return m_indexes; }
    std::optional<qint64> indexWh(const QByteArray &label) const;

    static Option optionFromRaw(const QByteArray &optarif);
    static Period periodFromRaw(const QByteArray &ptec);
    static QString optionCode(Option option);   // "base", "hchp", "ejp", "tempo", "unknown"
    static QString periodCode(Period period);   // "base", "hc", "hp", "hn", "pm", "hcjb" ... "unknown"
    static QString periodLabel(Period period);  // in clear, French: "Heures creuses" ...

    static const QList<QByteArray> &indexLabels();

private:
    int m_labels = 0;
    bool m_hasMeterNumber = false;
    Option m_option = Option::Unknown;
    QByteArray m_rawOption;
    Period m_period = Period::Unknown;
    QByteArray m_rawPeriod;
    std::optional<int> m_isousc;
    std::optional<int> m_papp;
    std::optional<int> m_iinst[4]; // [0] single phase, [1..3] three phase
    bool m_threePhase = false;
    bool m_adps = false;
    std::optional<int> m_pejp;
    TempoColor m_demain = TempoColor::Unknown;
    QHash<QByteArray, qint64> m_indexes;
};

Q_DECLARE_METATYPE(TicReading)

#endif // TICREADING_H
