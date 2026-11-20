// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026, ETM-Schurig SARL. This file is part of etm-powersync-plugins,
// distributed under the GNU General Public License v3 or later (see ticdecoder.h).

#include "ticdecoder.h"

namespace {
constexpr char STX = 0x02;
constexpr char ETX = 0x03;
constexpr char EOT = 0x04;
constexpr char LF = 0x0A;
constexpr char CR = 0x0D;
constexpr char SP = 0x20;
constexpr char HT = 0x09;
}

char TicDecoder::checksum(const QByteArray &label, const QByteArray &data)
{
    unsigned int sum = 0;
    for (char c : label)
        sum += static_cast<unsigned char>(c);
    sum += static_cast<unsigned char>(SP);
    for (char c : data)
        sum += static_cast<unsigned char>(c);
    return static_cast<char>((sum & 0x3F) + 0x20);
}

QByteArray TicDecoder::encodeGroup(const QByteArray &label, const QByteArray &data)
{
    QByteArray group;
    group.append(LF);
    group.append(label);
    group.append(SP);
    group.append(data);
    group.append(SP);
    group.append(checksum(label, data));
    group.append(CR);
    return group;
}

QByteArray TicDecoder::encodeFrame(const Frame &groups)
{
    QByteArray frame(1, STX);
    for (const Group &group : groups)
        frame.append(encodeGroup(group.first, group.second));
    frame.append(ETX);
    return frame;
}

void TicDecoder::resetCounters()
{
    m_framesDecoded = 0;
    m_badChecksums = 0;
    m_malformedGroups = 0;
    m_interruptedFrames = 0;
    m_standardGroups = 0;
}

void TicDecoder::clear()
{
    m_state = State::Idle;
    m_group.clear();
    m_frame.clear();
    m_frameBytes = 0;
}

QList<TicDecoder::Frame> TicDecoder::feed(const QByteArray &bytes)
{
    QList<Frame> frames;
    for (char raw : bytes) {
        const char c = static_cast<char>(raw & 0x7F);

        if (c == STX) {
            // A new frame starts: whatever was open is cut.
            if (m_state != State::Idle && (m_frameBytes > 0 || !m_frame.isEmpty()))
                m_interruptedFrames++;
            clear();
            m_state = State::InFrame;
            continue;
        }
        if (m_state == State::Idle)
            continue; // between frames, or joined in the middle of one: wait for STX

        if (c == EOT) {
            m_interruptedFrames++;
            clear();
            continue;
        }

        if (++m_frameBytes > MaxFrameBytes) {
            // No ETX where a real frame has one: line noise, or a 9600 baud stream read at 1200.
            m_interruptedFrames++;
            clear();
            continue;
        }

        if (c == ETX) {
            if (m_state == State::InGroup)
                m_malformedGroups++; // a group without its CR
            if (!m_frame.isEmpty()) {
                frames.append(m_frame);
                m_framesDecoded++;
            }
            clear();
            continue;
        }

        if (c == LF) {
            if (m_state == State::InGroup)
                m_malformedGroups++; // the previous group had no CR
            m_group.clear();
            m_state = State::InGroup;
            continue;
        }

        if (m_state != State::InGroup)
            continue; // bytes between CR and LF: not part of any group

        if (c == CR) {
            endGroup();
            m_state = State::InFrame;
            continue;
        }

        if (m_group.size() >= MaxGroupBytes) {
            m_malformedGroups++;
            m_group.clear();
            m_state = State::InFrame; // skip until the next LF
            continue;
        }
        m_group.append(c);
    }
    return frames;
}

void TicDecoder::endGroup()
{
    const QByteArray group = m_group;
    m_group.clear();

    if (group.contains(HT)) {
        m_standardGroups++;
        return;
    }
    // label SP data SP checksum : at least "L" SP SP "c" (empty data is legal)
    if (group.size() < 4 || group.at(group.size() - 2) != SP) {
        m_malformedGroups++;
        return;
    }
    const char received = group.at(group.size() - 1);
    const QByteArray body = group.left(group.size() - 2); // label SP data
    const int sp = body.indexOf(SP);
    if (sp <= 0) {
        m_malformedGroups++;
        return;
    }
    const QByteArray label = body.left(sp);
    const QByteArray data = body.mid(sp + 1);
    if (checksum(label, data) != received) {
        m_badChecksums++;
        return;
    }
    m_frame.append(Group(label, data));
}
