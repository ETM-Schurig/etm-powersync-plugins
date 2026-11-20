#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Negative controls of the edf tests: each protection of the plugin is removed in turn, in a
# copy of the sources, and the suite that guards it must then FAIL. A protection whose
# removal leaves the suite green is not guarded, and reported as such.
#
#   edf/tests/negative-controls.py [work directory, default /tmp/edf-negative] [name filter]
#
# A protection doubled on purpose is listed twice: removing one layer leaves the suite green
# ("SURVIVED (doubled)"), the mutation removing both must be killed.
#
# One build at a time (make -j2). The sources of the repository are never modified.

import os
import shutil
import subprocess
import sys

EDF = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/edf-negative"
FILTER = sys.argv[2] if len(sys.argv) > 2 else ""

DOUBLED = "(one layer of a doubled protection)"

# (name, suite, file, original, mutated)
MUTATIONS = [
    ("checksum verified", "tic", "ticdecoder.cpp",
     "if (checksum(label, data) != received) {", "if (false) {"),
    ("bit 7 masked", "tic", "ticdecoder.cpp",
     "const char c = static_cast<char>(raw & 0x7F);", "const char c = raw;"),
    ("EOT drops the frame", "tic", "ticdecoder.cpp",
     "if (c == EOT) {", "if (false) {"),
    ("frame size bounded", "tic", "ticdecoder.cpp",
     "if (++m_frameBytes > MaxFrameBytes) {", "if (++m_frameBytes < 0) {"),
    ("meter number not kept", "tic", "ticreading.cpp",
     "reading.m_hasMeterNumber = !data.isEmpty();", "reading.m_hasMeterNumber = !data.isEmpty(); reading.m_rawOption = data;"),
    ("numbers are digits only", "tic", "ticreading.cpp",
     "if (c < '0' || c > '9')\n            return std::nullopt;", "if (c < '0' && c > '9')\n            return std::nullopt;"),

    ("Tempo refused", "contract", "contract.cpp",
     "    case Type::Tempo:\n        *error", "    case Type::Tempo:\n        contract.m_periods = { QStringLiteral(\"base\") };\n        break;\n        *error"),
    ("off-peak ranges do not overlap", "contract", "contract.cpp",
     "if (used.testBit(minute)) {", "if (false) {"),
    ("entered price 0 is not a price", "contract", "contract.cpp",
     "if (!(price > 0) ||", "if (price < 0 ||"),
    ("nominal of a flat contract is 0", "contract", "contract.cpp",
     "if (high - low < 1e-9)\n        return 0;", "if (high - low < 1e-9)\n        return 100;"),
    ("nominal of a foreign period is 100", "contract", "contract.cpp",
     "if (!m_prices.contains(period))\n        return 100;", "if (!m_prices.contains(period))\n        return 0;"),
    ("regulated tariff to verify by default", "contract", "regulatedtariff.cpp",
     "toBool(true);", "toBool(false);"),
    ("boundary in the missing hour of DST", "contract", "priceschedule.cpp",
     "if (dateTime.time() == time && dateTime.date() == day)", "if (true)"),
    ("TIC overrides the slot in progress", "contract", "priceschedule.cpp",
     "if (!ticPeriod.isEmpty() && contract.hasPeriod(ticPeriod)) {", "if (false) {"),
    ("undeclared EJP day presumed normal", "contract", "ejpcalendar.cpp",
     "return Day { Status::Normal, Source::Presumed };", "return Day { Status::Peak, Source::Presumed };"),
    ("presumption for prices only, the counter keeps unknown", "contract", "ejpcalendar.cpp",
     "    return Day {};\n}", "    return Day { Status::Normal, Source::Presumed };\n}"),
    ("a peak seen on the meter stays a peak", "contract", "ejpcalendar.cpp",
     "if (previous == Status::Peak || previous == status)", "if (previous == status)"),
    ("peak window 00:00-01:00 belongs to the day before", "contract", "ejpcalendar.cpp",
     "return local.time() < QTime(7, 0) ? local.date().addDays(-1) : local.date();", "return local.date();"),

    ("TIC wins over a declaration", "contract", "ejpcalendar.cpp",
     "if (tic != Status::Unknown)\n        return Day { tic, Source::Tic };", ""),
    ("contradicted declaration published", "contract", "ejpcalendar.cpp",
     "if (!m_declared.contains(date) || m_tic.value(date, Status::Unknown) != Status::Normal)\n        return QString();",
     "return QString();"),
    ("declaration targets the day of the next 07:00", "contract", "ejpcalendar.cpp",
     "return ejpDayOf(local).addDays(1);", "return local.date().addDays(1);"),
    ("declaration out of season refused", "contract", "ejpcalendar.cpp",
     "if (!inSeason(day))\n        return DeclareResult::OutOfSeason;", ""),
    ("declaring twice changes nothing", "contract", "ejpcalendar.cpp",
     "if (m_declared.contains(day))\n        return DeclareResult::AlreadyDeclared;", ""),
    ("no cancel once the peak started", "contract", "ejpcalendar.cpp",
     "if (m_declared.remove(next)) {", "if (m_declared.remove(next) || m_declared.remove(ejpDayOf(local))) {"),
    ("declaration kept in the storage", "contract", "ejpcalendar.cpp",
     "map.insert(QStringLiteral(\"manuel\"), manual);", ""),
    ("declaration refused on a non-EJP contract", "contract", "ejpdeclaration.cpp",
     "Outcome declareTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local)\n{\n    if (type != Contract::Type::Ejp)",
     "Outcome declareTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local)\n{\n    if (false && type != Contract::Type::Ejp)"),
    ("cancel refused on a non-EJP contract", "contract", "ejpdeclaration.cpp",
     "Outcome cancelTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local)\n{\n    if (type != Contract::Type::Ejp)",
     "Outcome cancelTomorrow(Contract::Type type, EjpCalendar &calendar, const QDateTime &local)\n{\n    if (false && type != Contract::Type::Ejp)"),

    ("season: a day seen and entered is counted once", "contract", "ejpcalendar.cpp",
     "return applies ? qMax(start, m_countFrom) : start;", "return start;"),
    ("season: entered days for the season of the installation only", "contract", "ejpcalendar.cpp",
     "const bool applies = m_countFrom.isValid() && seasonStart(m_countFrom.addDays(-1)) == start;",
     "const bool applies = m_countFrom.isValid();"),
    ("season: installation season is the one before the count start", "contract", "ejpcalendar.cpp",
     "seasonStart(m_countFrom.addDays(-1)) == start", "seasonStart(m_countFrom) == start"),
    ("season: entered days bounded to 22", "contract", "ejpcalendar.cpp",
     "m_priorPeakDays = qBound(0, priorPeakDays, SeasonPeakDays);", "m_priorPeakDays = priorPeakDays;"),

    ("ISOUSC: three phase counts per phase", "contract", "contract.cpp",
     "return amps * 0.2 * (threePhase ? 3 : 1);", "return amps * 0.2;"),
    ("ISOUSC: a mismatch is published", "contract", "contract.cpp",
     "if (qAbs(kva - enteredKva) < 0.5)", "if (true)"),
    ("ISOUSC: an agreement is silent", "contract", "contract.cpp",
     "if (qAbs(kva - enteredKva) < 0.5)", "if (false)"),

    ("open is not connected", "port", "ticport.cpp",
     "    m_openedAt = QDateTime::currentDateTimeUtc();", "    m_openedAt = QDateTime::currentDateTimeUtc(); setConnected(true);"),
    ("silence gives disconnected", "port", "ticport.cpp",
     "if (m_connected && m_lastFrame.msecsTo(now) > m_staleAfterMs)", "if (false)"),
    ("absent port retried", "port", "ticport.cpp",
     "        port->deleteLater();\n        m_retryTimer.start(m_retryMs);", "        port->deleteLater();"),
    ("an error reported once per outage", "port", "ticport.cpp",
     "if (error == m_lastError)\n        return;", ""),
]


def run(command, cwd, timeout):
    try:
        return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        return -9


def main():
    results = []
    for name, suite, filename, original, mutated in MUTATIONS:
        if FILTER and FILTER not in name:
            continue
        tree = os.path.join(WORK, "src")
        if os.path.exists(tree):
            shutil.rmtree(tree)
        shutil.copytree(EDF, tree, ignore=shutil.ignore_patterns("*.o", "Makefile*", "moc_*"))
        edits = filename if isinstance(filename, list) else [(filename, original, mutated)]
        applied = True
        for edit_file, edit_original, edit_mutated in edits:
            path = os.path.join(tree, edit_file)
            text = open(path, encoding="utf-8").read()
            if text.count(edit_original) != 1:
                results.append((name, suite, "NOT APPLIED (pattern found %d times)" % text.count(edit_original)))
                applied = False
                break
            open(path, "w", encoding="utf-8").write(text.replace(edit_original, edit_mutated))
        if not applied:
            print(results[-1], flush=True)
            continue
        build = os.path.join(WORK, "build-" + suite)
        os.makedirs(build, exist_ok=True)
        if run(["qmake6", os.path.join(tree, "tests", suite, suite + ".pro")], build, 120) != 0 \
                or run(["make", "-j2"], build, 900) != 0:
            results.append((name, suite, "killed (does not build)"))
        else:
            binary = [f for f in os.listdir(build) if f.startswith("tst_") and not f.endswith((".o", ".cpp", ".moc"))][0]
            code = run(["./" + binary, "-silent"], build, 300)
            results.append((name, suite, "killed (suite fails)" if code != 0 else "SURVIVED (suite green)"))
        print(results[-1], flush=True)

    print()
    survivors = [r for r in results if not r[2].startswith("killed") and DOUBLED not in r[0]]
    for name, suite, verdict in results:
        print("%-50s %-9s %s" % (name, suite, verdict))
    doubled = [r for r in results if DOUBLED in r[0]]
    print("\n%d mutations, %d killed, %d single layers of doubled protections, %d not killed"
          % (len(results), len([r for r in results if r[2].startswith("killed")]), len(doubled), len(survivors)))
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
