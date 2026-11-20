#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-regression tests for the hourly integration convention of the openmeteo plugin.

FORM OF THESE TESTS — do not change it into something else.

CONTRACT_A section 2.2 requires that the convention be checked against terms that do not
share their origin with the plugin's own arithmetic. An identity whose two terms come
from the same source verifies nothing: it confirms the code agrees with itself, which
was never in doubt. Recomputing our own sum a second way, or comparing it to a constant
we chose, would establish nothing. Only a term produced outside our control is proof.

Two DIFFERENT properties are checked, because one test cannot cover both:

  Test 1 — integration and day bounds.
      The plugin's hourly sum over J 01:00 -> J+1 00:00 against Open-Meteo's own
      `daily.shortwave_radiation_sum`. This proves the hourly values are true hourly
      means and that our day boundaries are right.

      What it does NOT prove: whether each mean covers the preceding or the following
      hour. The day boundary falls at midnight, where irradiance is zero, so shifting
      the summation by one hour there changes no total. This was verified by deliberately
      breaking the convention and watching this test still pass. Test 1 alone is not
      evidence for the backward-mean convention.

  Test 2 — backward-mean convention (the discriminating one).
      `shortwave_radiation` (mean) against `shortwave_radiation_instant` (instantaneous),
      two series Open-Meteo publishes independently of us. If the mean at HH covers
      (HH-1, HH], it must track the midpoint of instant[HH-1] and instant[HH]; if it
      covered (HH, HH+1], it would track the midpoint of instant[HH] and instant[HH+1].
      Measured 2026-09-08: mean absolute error 26.0 W/m2 under the backward hypothesis
      against 83.2 under the forward one, a ratio of 0.31.

Usage:
    python3 check_integration_convention.py [--lat 48.85] [--lon 2.35]
Exit status 0 on success, 1 on failure. Requires network access.
"""

import argparse
import json
import sys
import urllib.parse
import urllib.request

# Test 1. The plugin publishes numbers biased by roughly 20 % (CONTRACT_A section 0.1),
# so this threshold is not about physical accuracy: it is tight enough that a broken
# integral cannot pass, loose enough to tolerate Open-Meteo's own rounding.
DAILY_TOLERANCE_PERCENT = 1.0

# Test 2. Backward must beat forward by a wide margin. Measured ratio is 0.31; 0.60
# leaves room for a more overcast day without admitting the wrong convention.
CONVENTION_MAX_RATIO = 0.60

# Below this irradiance the two hypotheses are indistinguishable (night, and the flat
# shoulders around sunrise and sunset).
CONVENTION_MIN_IRRADIANCE = 50.0

FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
MODEL = "meteofrance_seamless"


def fetch(lat, lon, days=3):
    params = {
        "latitude": f"{lat:.2f}",
        "longitude": f"{lon:.2f}",
        "models": MODEL,
        "timezone": "Europe/Paris",
        "hourly": "shortwave_radiation,shortwave_radiation_instant",
        "daily": "shortwave_radiation_sum",
        "forecast_days": str(days),
    }
    url = f"{FORECAST_URL}?{urllib.parse.urlencode(params)}"
    with urllib.request.urlopen(url, timeout=30) as response:
        payload = json.load(response)
    if payload.get("error"):
        raise SystemExit(f"API error: {payload.get('reason')}")
    return payload


def plugin_sum_for_day(times, values, day):
    """The plugin's convention: a value timestamped HH is the mean over (HH-1, HH].

    Calendar day `day` is therefore covered by its own 01:00..23:00 buckets plus the
    00:00 bucket of the next day.
    """
    total = 0.0
    for timestamp, value in zip(times, values):
        if value is None:
            continue
        date, clock = timestamp.split("T")
        if date == day and clock != "00:00":
            total += value
        elif date > day and clock == "00:00":
            total += value
            break
    return total


def test_daily_identity(payload):
    hourly, daily = payload["hourly"], payload["daily"]
    print("TEST 1 — integration and day bounds")
    print("  term A: plugin hourly sum   term B: Open-Meteo daily aggregate")
    print(f"  {'day':<12}{'A (MJ/m2)':>12}{'B (MJ/m2)':>12}{'delta':>9}")

    failures = checked = 0
    for index, day in enumerate(daily["time"]):
        reference = daily["shortwave_radiation_sum"][index]
        if reference is None or reference <= 0:
            continue
        # Wh/m2 -> MJ/m2, to compare in Open-Meteo's unit rather than converting theirs.
        mine = plugin_sum_for_day(hourly["time"], hourly["shortwave_radiation"], day) * 3600 / 1e6
        delta = 100.0 * (mine - reference) / reference
        checked += 1
        failed = abs(delta) > DAILY_TOLERANCE_PERCENT
        failures += failed
        print(f"  {day:<12}{mine:12.2f}{reference:12.2f}{delta:+8.2f}%{'   <-- FAIL' if failed else ''}")

    if checked == 0:
        print("  FAIL: no complete day to check.\n")
        return False
    if failures:
        print(f"  FAIL: {failures}/{checked} day(s) beyond {DAILY_TOLERANCE_PERCENT} %.\n")
        return False
    print(f"  PASS: {checked} day(s) within {DAILY_TOLERANCE_PERCENT} %.\n")
    return True


def test_backward_mean(payload):
    hourly = payload["hourly"]
    times = hourly["time"]
    mean = hourly["shortwave_radiation"]
    instant = hourly["shortwave_radiation_instant"]

    print("TEST 2 — backward-mean convention (discriminating)")
    backward = forward = 0.0
    samples = 0
    for i in range(1, len(times) - 1):
        window = (instant[i - 1], instant[i], instant[i + 1])
        if mean[i] is None or any(v is None for v in window):
            continue
        if max(window) < CONVENTION_MIN_IRRADIANCE:
            continue
        backward += abs(mean[i] - (instant[i - 1] + instant[i]) / 2.0)
        forward += abs(mean[i] - (instant[i] + instant[i + 1]) / 2.0)
        samples += 1

    if samples < 10 or forward <= 0:
        print(f"  FAIL: only {samples} usable samples.\n")
        return False

    ratio = backward / forward
    print(f"  samples: {samples}")
    print(f"  mean absolute error, backward hypothesis: {backward / samples:7.2f} W/m2")
    print(f"  mean absolute error, forward hypothesis:  {forward / samples:7.2f} W/m2")
    print(f"  ratio backward/forward: {ratio:.3f} (must be below {CONVENTION_MAX_RATIO})")
    if ratio > CONVENTION_MAX_RATIO:
        print("  FAIL: the hourly values no longer behave as preceding-hour means.")
        print("  Forward windows must sum t+1..t+N; see CONTRACT_A section 2.2.\n")
        return False
    print("  PASS\n")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lat", type=float, default=48.85)
    parser.add_argument("--lon", type=float, default=2.35)
    args = parser.parse_args()

    payload = fetch(args.lat, args.lon)
    print(f"Site {args.lat:.2f}/{args.lon:.2f}, model {MODEL}\n")

    results = [test_daily_identity(payload), test_backward_mean(payload)]
    if all(results):
        print("ALL PASS")
        return 0
    print("FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
