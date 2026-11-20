# Generic energy devices

This plugin allows to create generic energy related things in nymea. Such things don't have any
functionality on their own, but allow to create a visually appealing experience when
using general purpose input and outputs, such as GPIOs or MQTT.

## Set-up

First, set up a thing with generic inputs or outputs. For instance from the GPIO or MQTT plugins.
When done, set up the desired generic thing and use the things settings to connect
the inputs and/or outputs or nymea scripts to fill in the thing states accordingly.

## Differences from upstream (ETM fork)

This copy is packaged as `powersync-plugin-generic-energy`, which provides, replaces and
conflicts with `nymea-plugin-generic-energy`. It starts from upstream nymea-plugins 1.15.2,
imported unchanged, with three changes on top:

1. **Impulse smart meter removal.** Both branches of `thingRemoved()` tested the same thing
   class, so the branch that destroyed the impulse timer was unreachable: every removed
   impulse smart meter left a running timer writing to a removed thing. The two branches are
   merged. Upstream defect; the fix is *to be proposed* upstream (not yet submitted).
2. **Energy integration for the smart meter consumer.** `totalEnergyConsumed` was only ever
   written by its action, so it stayed frozen on the last value set, and read as a valid
   measurement of zero consumption.
3. **Energy integration extended** to the smart meter producer, the energy meter and the
   wallbox, which had the same frozen counters.

How the integration works:

- Every second, `currentPower` is integrated over the time *actually* elapsed since the last
  tick, not the nominal period.
- The sign picks the counter, as the nymea `smartmeterproducer` interface defines it
  (power is seen from the consumer side): positive power adds to `totalEnergyConsumed`,
  negative power adds its absolute value to `totalEnergyProduced`. A thing without the
  matching counter (a producer has no consumed counter) is not credited at all.
- Counters only ever increase.
- When an action writes a counter of a thing, the integration of that thing pauses for
  15 minutes, so an external source feeding the counter is not counted twice. The pause
  expires, so a one-off reset does not freeze the counter again.
- Counters are cached states: after a restart of nymead they resume from their last value;
  the downtime is not credited.
- The impulse smart meter is left out on purpose: it already counts its energy impulse by
  impulse. The energy storage and the socket have no energy counter.

Changes 2 and 3 are ETM behaviour and are not currently planned for upstream.
