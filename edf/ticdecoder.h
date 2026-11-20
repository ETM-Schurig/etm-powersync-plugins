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

#ifndef TICDECODER_H
#define TICDECODER_H

#include <QByteArray>
#include <QList>
#include <QPair>

// Decoder of the Linky / electronic meter "téléinformation client" (TIC), historic mode
// (Enedis-NOI-CPT_02E): 1200 baud, 7 data bits, even parity, 1 stop bit.
//
// Frame:  STX (0x02)  group*  ETX (0x03)        EOT (0x04) = the frame was interrupted
// Group:  LF (0x0A) label SP data SP checksum CR (0x0D)
// Checksum (historic mode): (sum of the bytes of label, the first SP and data) & 0x3F + 0x20.
// The checksum byte can itself be a space (0x20): it is the last byte before CR, never split on.
//
// Bit 7 of every byte is masked: read with 8N1 (Raspberry Pi mini-UART, which has no parity)
// the parity bit lands there; read with 7E1 it is already 0. The masking is unconditional,
// so the decoder gives the same frames in both cases.
//
// A group with a wrong checksum is counted and dropped; the rest of the frame is kept
// (the meter repeats every group in every frame, about every 1.5 s). A frame cut by EOT, by
// a new STX, or grown beyond any real frame, is dropped whole and counted.
//
// A group separated by HT (0x09) is a standard-mode group (9600 baud, other labels); it is
// not decoded but counted, so that the plugin can say "standard mode" instead of "garbage".
class TicDecoder
{
public:
    using Group = QPair<QByteArray, QByteArray>; // label, data
    using Frame = QList<Group>;

    // Feeds raw bytes from the port; returns the frames completed by these bytes (ETX seen).
    QList<Frame> feed(const QByteArray &bytes);

    // Checksum of one historic group (label SP data).
    static char checksum(const QByteArray &label, const QByteArray &data);
    // One encoded group, LF..CR, as a meter sends it. For the tests and the fake meter.
    static QByteArray encodeGroup(const QByteArray &label, const QByteArray &data);
    static QByteArray encodeFrame(const Frame &groups);

    // Counters since construction (or reset()).
    int framesDecoded() const { return m_framesDecoded; }
    int badChecksums() const { return m_badChecksums; }
    int malformedGroups() const { return m_malformedGroups; }
    int interruptedFrames() const { return m_interruptedFrames; }
    int standardGroups() const { return m_standardGroups; }
    void resetCounters();

    // Drops any partial frame (the port was closed and reopened).
    void clear();

    static constexpr int MaxFrameBytes = 2048;  // a three-phase historic frame is ~ 400 bytes
    static constexpr int MaxGroupBytes = 64;

private:
    void endGroup();

    enum class State { Idle, InFrame, InGroup };
    State m_state = State::Idle;
    QByteArray m_group;
    Frame m_frame;
    int m_frameBytes = 0;

    int m_framesDecoded = 0;
    int m_badChecksums = 0;
    int m_malformedGroups = 0;
    int m_interruptedFrames = 0;
    int m_standardGroups = 0;
};

#endif // TICDECODER_H
