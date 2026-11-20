# V2C Trydan (HTTP)

This plugin drives V2C Trydan EV chargers through their local HTTP API:
`GET /RealTimeData` to read, `GET /write/<Param>=<integer>` to write. It is the one to
use for single / three phase switching, which the Modbus map of the charger does not
offer.

> **One controller per charger.** Do not drive the same Trydan with this plugin and the
> Modbus V2C plugin (or the V2C online service, its app, another energy manager) at the same
> time. Two masters fight over the setpoint, and the Wi-Fi of the charger degrades under
> concurrent requests.

## Setup

The charger is added by discovery or by entering its IPv4 address. It needs a fixed
(DHCP-reserved) address.

Discovery lists the hosts of the local network and sends each one a single
`GET /RealTimeData` (3 s timeout, no retry, never a write). A host is kept when its reply
carries the Trydan fields `ChargeState`, `ChargePower`, `Paused`, `Intensity` and
`MaxIntensity`.

At setup the plugin reads the charger once. A device that answers but not like a Trydan
is refused. A charger that does not answer (off, Wi-Fi down while nymea starts) is set up
disconnected, and comes up with the polling.

### Wiring (set by the installer)

The charger does not report its wiring on its local API, so the installer chooses it
when adding the charger, from what is installed - the Trydan model and its supply:

* **Single phase - 1 x 230 V, up to 7.4 kW**: phase switching is disabled and three
  phases are never announced.
* **Three phase - 3 x 400 V, up to 22 kW**: phase switching is offered when the firmware
  reports `ChargeMode`.

There is no default. The first entry of the list, *Choose the wiring*, is not a wiring:
setup is refused with it ("Choose the wiring of the charger"). A rewiring is entered by
reconfiguring the charger. (The V2C online API keeps the same fact as the installation
type of the charger, `inst_type`; it is not read here: this plugin uses the local API
only.)

**Wiring check.** When the firmware reports the per-phase voltages, the wiring is
compared with the voltages on L2 and L3 and the verdict published in `wiringCheck`:
mains voltage (>= 150 V) on L2 or L3 of a charger set as single phase, or none (<= 50 V)
on a charger set as three phase, is a mismatch, logged as a warning. The second one only
while the charger is in three-phase mode (or its firmware has no phase mode): in
single-phase or mixed mode the charger leaves L2/L3 open itself, so no voltage there says
nothing of the wiring. In between, or without the per-phase fields, or with L1 not at
mains voltage, or no voltage on L2/L3 outside three-phase mode: not checked. The check is
information only; nothing driven depends on it.

### Settings

* **Suspend the charger's own power control** - default on. See below.

## States

| State | From |
|---|---|
| `connected` | false after 3 failed exchanges in a row, true at the next successful read |
| `pluggedIn` | `ChargeState` 1 or 2, or a current flowing (an unknown value keeps the previous one) |
| `charging` | plugged in **and** `currentPower` >= 100 W. Not `ChargeState` alone: it stays at 2 after a pause. Always true while a phase carries current |
| `power` | `Paused` == 0 |
| `maxChargingCurrent` | `Intensity`; range [max(6, `MinIntensity`), max(min, `MaxIntensity`)] |
| `currentPower` | `ChargePower` (W) while it agrees with the phase currents; otherwise (absent, or contradicting them) the sum of current x voltage over the phases. See below |
| `currentPhaseA/B/C` | `IntensityMeasure_L1..L3` (A), same reading as `currentPower`; below 1 A, exactly 0 |
| `voltagePhaseA/B/C` | `VoltageMeasure_L1..L3` (V) |
| `phaseMeasurementsAvailable` | the per-phase fields are sent and populated (not on every firmware) |
| `phaseCount` | while charging: the phases carrying current; otherwise 1 in single-phase mode, else the wiring |
| `desiredPhaseCount` | `ChargeMode` 0 -> 1, 1 -> 3. Mixed mode: unchanged. Single-phase wiring: frozen at 1, possible values {1}. No `ChargeMode` sent: frozen at the wiring |
| `chargeMode` | `ChargeMode` as text: Single phase, Three phase, Mixed, Unknown |
| `wiringCheck` | the wiring set against the voltages on L2/L3, see above |
| `sessionEnergy`, `totalEnergyConsumed` | built from `ChargeEnergy`, see below |
| `chargeEnergy` | `ChargeEnergy` as read |
| `locked`, `internalOptimizerActive`, `optimizerSuspended`, `slaveError`, `firmwareVersion` | `Locked`, `Dynamic`, `PauseDynamic`, `SlaveError`, `FirmwareVersion` |

**One reading, one story.** In a transition the charger's `ChargePower` and
`ChargeState` move before or after its measured phase currents: read right after a
pause, `ChargePower` may already be 0 (or the power of the new setpoint) while the
currents still carry the full charge, which the house meter confirms. The measured
currents make faith: when the per-phase fields are sent, `ChargePower` is published only
while it agrees with the sum of current x voltage (same side of 100 W, and within 20 % or
300 W), otherwise that sum is (nominal 230 V for a phase carrying current without mains
voltage on its reading). `charging`, `currentPower`, `currentPhaseA/B/C` and `phaseCount`
then never contradict each other on a reading. A firmware without the per-phase fields
publishes `ChargePower` as read.

**Freshness.** The states are those of the last reading: the 30 s poll, or the read back
of the last write. Between two readings nothing changes, whatever the charger does.

## Actions

Each action is a single gesture: write, require the answer `OK`, read `RealTimeData`
back (twice at most, 1 s apart) and succeed only if the charger reports the written
value. A write whose value the last reading already shows succeeds without writing; this
is decided when the write leaves the queue, on what the previous gesture left.

* `power` -> `Paused` (0 resumes, 1 pauses).
* `maxChargingCurrent` -> `Intensity`, rounded down (never more than asked) and capped
  at the maximum of the charger. **Below the minimum** (6 A, or `MinIntensity` when the
  charger asks for more) the charger does not work: charging stops - `Paused` = 1, and
  `Intensity` is not written. It is never set to the minimum instead, which would draw
  more than asked. A setpoint back above the minimum writes `Intensity` and leaves the
  pause as it is: resuming is the `power` action. Note that nymea itself refuses an
  action value under the published minimum of the state, which is that same minimum.
* `desiredPhaseCount` -> `ChargeMode` (0 single, 1 three phase). Refused with 3 on a
  single-phase wiring, and on a firmware that does not send `ChargeMode`. Not blocked
  while charging, but logged: what the charger does with a phase change under load is not
  documented, and the energy manager is expected to pause first.
* `refresh` - read the charger now.

The plugin does not chain gestures. Switching phases while charging is a sequence -
down to the minimum current, pause, switch, resume - and that sequence belongs to the
energy manager.

## Requests to the charger

One request at a time, in arrival order: actions sent together are queued, never sent
in parallel. Polling every 30 s; a poll tick finds anything queued or running and is
skipped, so polls never pile up. Each request times out after 4 s, so one gesture lasts
13 s at most, within the 15 s nymea gives an action. A gesture whose action nymea has
given up on is dropped if it was not sent yet.

## The charger's own power control

With `Dynamic` = 1 the charger runs its own power control, which overrides the
`Intensity` setpoint. While the setting allows it, the plugin suspends it with
`PauseDynamic` = 1 - written only when `Dynamic` reads 1 and `PauseDynamic` does not,
retried at most every 10 minutes. It is left suspended through pauses. `Dynamic` itself
is never written: without it the charger stops reporting its power. With the setting off,
the plugin does not touch `PauseDynamic`.

## Energy

`ChargeEnergy` is reset by the charger whenever a charge stops, including every pause,
and it has no lifetime total. `sessionEnergy` and `totalEnergyConsumed` are therefore
accumulated from its increments: a fall means a reset, and what was counted since is
added. A session opens when a vehicle is plugged in and closes after three successful
polls in a row without a vehicle; failed polls do not count, so a dead link cannot close a
session. The energy delivered between the last poll and a reset is lost (at most one poll
interval).

## Tests

Offline, not part of the package build (`qmake6 && make && ./tst_...` in each directory):

* `tests/model` - parsing and the state rules, including the wiring choices read from
  the metadata.
* `tests/energy` - session and total energy.
* `tests/client` - the HTTP client against a local fake charger
  (`tests/common/faketrydan.*`): switch taken and read back, refused, timeout, not
  taken, firmware without `ChargeMode`, disconnection and recovery, pause with
  `ChargeState` still at 2, mixed mode, requests in series, polls not piling up, the
  6 A -> pause -> switch -> resume sequence, and a setpoint below the minimum (pause
  while charging, nothing started when stopped, pause kept when the setpoint comes back).
