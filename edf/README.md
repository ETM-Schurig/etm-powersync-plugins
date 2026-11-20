# EDF: electricity contract and Linky meter

This plugin has two things:

* **Compteur Linky (TIC)** reads the customer information output (TIC) of a Linky meter
  in **historic mode** on a serial port and publishes what the meter says.
* **Contrat d'électricité** describes the contract (Base, off-peak / peak hours, EJP) and
  publishes the price of the period in progress, its nominal price 0-100, the next switch,
  the EJP days, and the price slots up to the end of the next day. It follows a linked Linky
  for the option and the period in progress.

Tempo is reserved: a contract set to Tempo is refused with a clear message for now.

Everything is computed on the box from the contract and the meter. **The plugin opens no
network connection**: it does not link QtNetwork and asks nothing of any service.

## Compteur Linky (TIC)

### Wiring and port

The TIC output (terminals I1/I2 of the meter) goes through an optocoupler to a UART
(Raspberry Pi GPIO) or a USB serial adapter. The thing takes the **path of the serial port**:
`/dev/serial0`, `/dev/ttyAMA0`, `/dev/ttyUSB0`, or the link of the simulator.

The meter sends 1200 baud, 7 data bits, even parity, 1 stop bit. Port setting:

* **8N1, bit 7 masked** (default). Works on every UART. The Raspberry Pi mini-UART
  (`/dev/serial0` on a Pi 3 or Zero 2 W with Bluetooth on) has no parity: read as 8N1, the
  parity bit lands in bit 7, which the decoder masks. The checksum of every group still
  guards the data.
* **7E1**: the line as specified, for a UART with parity (PL011, USB adapters).

The decoder masks bit 7 in both cases, so both settings give the same frames on a UART that
supports 7E1.

### Frames

Historic mode (Enedis-NOI-CPT_02E): frame `STX (0x02) ... ETX (0x03)`, group
`LF label SP data SP checksum CR`, checksum = (sum of the bytes of label, SP and data) & 0x3F
+ 0x20. `EOT (0x04)` interrupts a frame.

* A group with a wrong checksum is dropped and counted (`badChecksums`); the rest of the
  frame is kept.
* A frame cut by EOT, by a new STX, or longer than any real frame, is dropped whole and
  counted (`interruptedFrames`).
* `connected` means **valid frames received in the last 10 s**, not "port open". A port that
  cannot be opened, disappears or goes silent gives `connected = false`; the port is retried
  every 10 s, and reopened after 60 s of silence (one log line per outage).

Labels read: `ADCO` (presence only), `OPTARIF`, `ISOUSC`, `BASE`, `HCHC`, `HCHP`, `EJPHN`,
`EJPHPM`, `BBRHCJB` ... `BBRHPJR`, `PEJP`, `PTEC`, `DEMAIN`, `IINST`, `IINST1..3`, `PAPP`,
`ADPS`, `ADIR1..3`, `IMAX1..3` / `PMAX` / `PPOT` (three-phase detection only). Ignored:
`IMAX`, `HHPHC`, `MOTDETAT`.

**The meter number (`ADCO`) is personal data**: it is never kept, logged or published.

### States

| State | Meaning |
|---|---|
| `connected` | valid frames received recently |
| `ticMode` | `aucune trame`, `historique`, `standard (non pris en charge)` |
| `contractOption` | `OPTARIF`: `base`, `hchp`, `ejp`, `tempo`, `unknown` |
| `currentPeriod` / `currentPeriodLabel` | `PTEC` as a code (`hc`, `pm`, `hcjb` ...) and in clear (`Heures creuses`) |
| `ejpNotice` / `ejpNoticeMinutes` | `PEJP` present, and its value |
| `index*` | indexes in kWh (`BASE`, `HCHC`, `HCHP`, `EJPHN`, `EJPHPM`, Tempo) |
| `currentPhaseA/B/C` | `IINST` (single phase) or `IINST1..3`, A |
| `apparentPower` | `PAPP`, **VA, not W** |
| `subscribedCurrent` | `ISOUSC`, A |
| `overcurrentWarning` | `ADPS` / `ADIR1..3` present |
| `tempoTomorrow` | `DEMAIN` (Tempo meters) |
| `badChecksums`, `interruptedFrames` | counters since the port was set up |

**Not an energy meter.** The thing has no `energymeter` interface: `PAPP` is apparent power
in VA, the historic mode has no export index, and a grid meter that appears without a gesture
must never become the root meter of the energy manager.

### Standard mode (later)

Not decoded yet. A standard-mode meter talks at 9600 baud with HT (0x09) separators, labels
such as `ADSC`, `NGTF`, `LTARF`, `EAST`, `SINSTS`, and a checksum that includes the separator
before it. At 1200 baud it reads as noise: no valid frame, `connected = false`. The decoder
already counts HT-separated groups; the standard mode will be a second choice of the
"Mode de la TIC" parameter (9600 baud), and an automatic attempt at 9600 baud when nothing
valid arrives at 1200.

## Contrat d'électricité

### Parameters

| Parameter | |
|---|---|
| Option du contrat | Base, Heures creuses / heures pleines, EJP, Tempo (refused for now) |
| Prix | regulated tariff shipped with the package, or prices entered |
| Puissance souscrite (kVA) | the regulated Base price depends on it (<= 6 kVA / >= 9 kVA) |
| Plages d'heures creuses | box local time, `22:00-06:00` or `12:00-14:00, 22:00-04:00`; HC/HP only |
| Prix saisis | c€/kWh TTC, per period, when "prix saisis" is chosen; 0 = not entered |
| EJP : jours de pointe déjà passés | 0-22, default 0: peak days of the season already passed when the contract is set up (the one in progress included) |
| Compteur Linky lié | id of a Linky thing, set by the discovery; empty = none |

The regulated tariff is `tariffs/trv-edf.json`, compiled into the plugin. It carries its
effective date, its source and a `toVerify` flag, published in `regulatedTariffDate`,
`regulatedTariffToVerify` and `tariffSource`. It changes only with the package.

### Link with the Linky

The contract is linked to a meter by the parameter **Compteur Linky lié**, filled by the
discovery of the contract class (one entry per Linky set up, the option read on it
preselected). It is not a parent/child relation: removing the meter must not remove the
contract, and a contract without a meter is legitimate.

When the linked meter sends valid frames and its `OPTARIF` matches the contract:

* the period in progress comes from `PTEC` (it overrides the off-peak ranges for the slot in
  progress; a disagreement is published in `discrepancy`);
* for EJP, `PEJP` and `PTEC = PM..` make the day a peak day, `PTEC = HN..` after 07:05 a
  normal one.

* `ISOUSC` is compared with the subscribed power entered, at 5 A per kVA: single phase 30 A =
  6 kVA, 45 A = 9 kVA, 60 A = 12 kVA; three phase, `ISOUSC` is the current of one phase, so
  15 A = 9 kVA, 20 A = 12 kVA (x 3). A mismatch is published in `subscribedPowerDiscrepancy`;
  nothing is corrected, the power entered stays the one used for the price.

At setup, a contract whose option differs from the option read on the linked meter is
refused ("Le compteur Linky lié lit l'option EJP, le contrat choisi est Base ..."). If the
meter reads another option later, the TIC is ignored and `linkyStatus` says so.

### EJP without TIC

Known from the contract alone: the mobile peak is 07:00 to 01:00 the day after, at most 22
days from 1 November to 31 March, never outside the season, and 01:00-07:00 is always normal
hours. Not known: which days. Without TIC and without a declaration (below), 07:00-01:00 of
a day in season is **presumed normal** (source `presume`, normal-hours price, nominal 0): the
TIC gives no notice the day before, so the declaration is the only way to know a peak in
advance, and about 130 of the 151 days of a season are normal. A peak neither declared nor
read becomes known at 06:30 (`PEJP`) or 07:00 (`PTEC`) on a linked meter. The season counter
does not count a presumed day: it stays unknown there (`ejpSeasonUnknownDays`).

### EJP: manual declaration

EDF publishes no EJP calendar a program may read, and the plugin opens no network
connection: the only sources are the TIC of the meter and the user. After the alert of EDF
(SMS, e-mail), the user tells the box from the app with the actions of the contract thing:

| Action | |
|---|---|
| `declareEjpTomorrow` — « Déclarer demain jour EJP » | no parameter |
| `cancelEjpTomorrow` — « Annuler la déclaration de demain » | no parameter |

* **EJP contract only**; on another contract both actions fail with « La déclaration d'un
  jour EJP ne concerne que le contrat EJP ... ».
* **Target**: the next EJP day whose peak has not started, i.e. the day of the **next 07:00**.
  From 00:00 to 07:00 that is the calendar day in progress (at 00:30 on the 21st, "tomorrow"
  is the peak of the 21st, 07:00 to 01:00 on the 22nd), not the 22nd.
* A target outside 1 November - 31 March is refused (« ... hors de la saison EJP ... »).
* Declaring twice changes nothing; cancelling when nothing is declared changes nothing.
* **Cancelling is refused from 07:00 of the declared day** (and until 07:00 the day after):
  the peak has started, the TIC of the meter decides.
* A declared day is a peak day, source `manuel`, in the slots (07:00-01:00 at the peak
  price), in `ejpToday` / `ejpTomorrow` and their source, and in `currentSource`. It is kept in
  the plugin storage and read again after a restart.

**The TIC wins on the day itself**: `PEJP` (notice) or `PTEC = PM..` confirm a declared day
(source `tic`); `PTEC = HN..` after 07:05 on a declared day **contradicts** it: the day becomes
normal and `discrepancy` says « déclaré jour EJP, la TIC lit des heures normales (déclaration
contredite, jour normal) ». A peak seen on the meter on a day nobody declared is a peak.

### States

| State | Meaning |
|---|---|
| `currentPeriod` / `currentPeriodLabel` | `base`, `hc`, `hp`, `hn`, `pm`, `unknown` |
| `currentMarketPrice` | c€/kWh TTC of the period in progress |
| `rank` | **nominal price 0-100**: 0 = cheapest period of the contract, 100 = dearest (min and max of the periods of the contract, not of the day). Base: 0. HC/HP: HC 0, HP 100. EJP: HN 0, PM 100; a normal day stays at 0 |
| `priceKnown`, `currentSource` | `contrat`, `tic`, `manuel`, `saison`, `presume` |
| `nextSwitch` | next change of period, ISO 8601 in the box local time with its offset; empty if none before the end of tomorrow |
| `validUntil` | the same as a Unix time (end of tomorrow when no switch) |
| `ejpToday`, `ejpTomorrow` (+ `Source`) | `normal` / `pointe` / `inconnu`, from `tic`, `manuel`, `saison` or `presume` (neither read nor declared: normal). "Today" is the EJP day in progress: from 00:00 to 07:00 it is still the day before (its peak runs until 01:00) |
| `ejpSeasonPeakDays` | peak days of the season (out of 22) = days entered + days seen by the box; a day counts from the start of its peak (07:00), not while it is only declared or announced |
| `ejpSeasonCountStart` | first day counted by the box this season (the days before are the number entered) |
| `ejpSeasonUnknownDays`, `ejpSeasonComplete` | days counted by the box, up to yesterday, without a known status; the count is a lower bound while some remain |
| `discrepancy` | TIC against a declaration (EJP), or TIC against the off-peak ranges (HC/HP) |
| `subscribedPowerDiscrepancy` | `ISOUSC` of the linked meter against the subscribed power entered, or "" (nothing is corrected) |
| `linkyStatus` | in clear |
| `priceSlotsTransport` | the price slots, below |

### EJP season counter

It comes from the TIC and the declarations only, plus the number entered at installation:

    ejpSeasonPeakDays = entered + peak days (TIC or declaration) from countFrom to today

`countFrom` is the day of the next 07:00 after the **first** setup as an EJP contract (kept in
the plugin storage; a reconfiguration does not move it). Every EJP day whose peak had started
at installation is in the number entered, every later day is counted by the box: no day is
counted twice (installed at 15:00 during a peak, that peak is entered, and the meter seeing it
afterwards does not add it; installed at 00:30, the peak of the day before, still on the
meter until 01:00, is entered too). The number entered applies to the season of the
installation only; the next season starts from zero. Days the box counts and did not see (box
off, no TIC, nothing declared) are unknown and make the count a lower bound
(`ejpSeasonComplete = false`).

The EJP calendar (the days seen on the meter and the days declared) is kept in the plugin
storage: after a restart the season counter comes back from there.

### Price slots: `priceSlotsTransport`

A JSON object (state of type `QVariantMap`), deterministic, recomputed every minute and on
every change of the meter, republished only when its content changes:

```json
{
  "schema": "etm.edf.priceSlots",
  "version": 1,
  "generatedAt": "2026-11-20T10:00:00+01:00",
  "timeZone": "Europe/Paris",
  "contract": "ejp",
  "priceSource": "regulated",
  "unit": "c€/kWh TTC",
  "periods": { "hn": { "price": 17.89, "nominal": 0 }, "pm": { "price": 43.24, "nominal": 100 } },
  "horizonEnd": "2026-11-22T00:00:00+01:00",
  "slots": [
    { "start": "2026-11-20T07:00:00+01:00", "end": "2026-11-21T01:00:00+01:00",
      "period": "hn", "price": 17.89, "nominal": 0, "source": "tic" },
    { "start": "2026-11-21T01:00:00+01:00", "end": "2026-11-21T07:00:00+01:00",
      "period": "hn", "price": 17.89, "nominal": 0, "source": "contrat" },
    { "start": "2026-11-21T07:00:00+01:00", "end": "2026-11-22T00:00:00+01:00",
      "period": "hn", "price": 17.89, "nominal": 0, "source": "presume" }
  ]
}
```

* Slots are contiguous, the first one contains `generatedAt`, the last one ends at
  `horizonEnd` (midnight at the end of tomorrow, box local time). Adjacent slots with the same
  period and source are merged; slots are as long as the periods (no hourly grid).
* Times are ISO 8601 with their UTC offset: unambiguous on the days of the clock change (a
  23 h or 25 h day falls right). Wall-clock boundaries are placed day by day in the box time
  zone; a boundary inside the missing hour of the spring change starts when the clock jumps.
* `price` and `nominal` are always set; a consumer still treats `null` as unknown, never 0.
* `source`: `contrat` (single price, off-peak ranges, EJP 01:00-07:00), `tic` (period in
  progress read on the meter, or EJP day seen on the meter), `manuel` (EJP day declared by the user),
  `saison` (EJP out of season), `presume` (EJP day in season neither read nor declared:
  presumed normal).
* A change of meaning gives a new `version`; consumers refuse a version they do not know.

## Simulator

`tools/etm-powersync-tic-simulateur` (Python 3, standard library) opens a pseudo-terminal,
links it to a stable path (default `/tmp/etm-tic`) and sends historic frames at the pace of
a 1200 baud line. Give the link as the serial port of the thing.

    tools/etm-powersync-tic-simulateur --scenario hchp --hc "12:00-14:00,22:00-04:00"
    tools/etm-powersync-tic-simulateur --scenario ejp --jour-ejp pointe --debut 2026-11-20T06:25
    tools/etm-powersync-tic-simulateur --scenario ejp --jour-ejp pointe --phases 3 --vu-par 8n1 --somme-fausse 5
    tools/etm-powersync-tic-simulateur --help

Scenarios Base, HC/HP (ranges as a parameter), EJP (normal days, peak days, or a list of
peak dates: notice 06:30-07:00, peak 07:00-01:00), single or three phase; simulated clock
with `--debut` and `--acceleration`; indexes advancing with `--puissance`; faults
(`--somme-fausse N`, `--coupe N`, `--silence-apres S --silence-duree S`); `--dump N` writes
frames to stdout for the tests. `--vu-par 8n1` puts the parity in bit 7, as a mini-UART sees
the line. The meter number is fictitious (`000000000000`).

The simulated clock drives the meter only: the contract dates what it reads with the box
clock, so an accelerated EJP day exercises the meter thing, not the calendar.

`tools/etm-powersync-tic-simulateur.service` is a trial systemd unit (link
`/run/etm-tic-sim/tic`), never installed nor enabled by the package of the plugin.

## Tests

Offline, no meter, no serial hardware, no network:

    tests/run-tests.sh            # the three suites: tic, contract, port
    tests/negative-controls.py    # each protection removed in turn: its suite must fail
