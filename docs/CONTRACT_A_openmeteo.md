# CONTRACT_A — openmeteo (v4)

*v4 — `1.15.2+etm16` (2026-10-15): scalar states temperature-corrected along with the curve
(§2.1, §2.6); altitude on the plane requests (§3.4); retry with doubling delay (§3.1);
series `observedGtiSeries` (§1.7) and `forecastHourlyWeather` (§1.8); spread timestamped and
invalidated (§1.5). Versions of the structured payloads in §5.10. v3 — `etm15`: transport v2.*

Output contract of the **openmeteo** plugin (GPL, repo `etm-powersync-plugins`) towards its
two consumers:

1. **The app** (display: site weather, solar trend of the day) — main consumer
   in V1.
2. **The engine** (`etm-powersync-energy-plugin-etm`) — passed through as is in the `forecast`
   field of the `SurplusContext` (see OPTIMIZER_PROTOCOL §5). The V1 rule-based engine reads
   nothing from it; an optimizer (ETM optimizer, third party) consumes it freely.

The numerical values and API behaviours cited here are established by real probes
documented in [OPENMETEO_API_PROBE.md](OPENMETEO_API_PROBE.md) (2026-09-08).

Principles:

- The plugin calls the **Open-Meteo API directly**. No dependency on a private ETM
  service: it must work on a pure Community installation.
- **No learning, no local correction** (K_climat / PR_site / K_dyn = ETM optimizer
  roadmap, outside this contract). The day-ahead is deliberately coarse, and
  declared as such — see §0.
- The plugin **relays**, it does not model. The only arithmetic allowed is a
  linear scaling by a parameter declared by the installer (§2.1) — and the
  module temperature correction by a NOCT model with declared parameters (§2.6,
  decision of 2026-10-12), applied since `etm16` to the curve and to the scalar states
  together.
- Fresh data or nothing: a doubtful field is `null` + `valid:false`, never an
  invented or stale value presented as fresh.

---

## 0. Regime: this is a forecast, never a measurement

**Central rule of the contract.** Everything this plugin publishes is a weather
forecast converted by rule of three. No state may be mistaken for a quantity
measured on the site.

Three mechanisms, cumulative, because a single one gets bypassed through inattention:

1. **The name carries the regime.** Every state is prefixed `forecast`. There is no
   `pvPower` state in this plugin, only `forecastPvPower`. A flag
   `regime:"forecast"` is silently ignored; a name is not. This is the main
   protection — the other two complement it, they do not replace it.
2. **The origin is published.** `forecastModel` says which model actually
   answered (§3.2), `forecastIssuedAt` which run the data comes from, `updateTime` when
   the reply was received. A forecast whose source and age are unknown is not
   auditable. Details in §3.3.
3. **The known deviations are listed in the contract**, below, and repeated in the
   transport (`corrections`, the list of what has been corrected — §5.9).

### 0.1 What the conversion ignores, explicitly

`forecastPvPower` [W] = GTI × `kwp` × `performanceRatio`, × the temperature factor of
§2.6 when the run is corrected. This conversion **integrates nothing** else of what follows,
and the deviation always goes in the optimistic direction:

| ignored factor | direction of the error | where it will be handled |
|---|---|---|
| site shading (trees, chimney, buildings) | overestimation, mostly at low sun hours | ETM optimizer (learned) |
| soiling / ageing of the modules | overestimation, increasing | ETM optimizer (learned) |
| module temperature (efficiency drops in summer) | overestimation, maximal under hot clear sky | **corrected here** (§2.6), in the transport curve v2 when `corrections` contains `temperature`, and since `etm16` in the scalar states of the same run |
| inverter clipping | overestimation at peaks | `ems`, **site** AC cap applied after the sum of the planes — never here |
| snow, leaves, occasional dirt | overestimation | not handled |

#### Quality varies with the horizon — but not the bias

The `seamless` model serves the first ~5 hours from a 15-minute AROME domain re-initialised
**every hour**, against 3 h for the hourly data (§3.3). The **meteorological** error is
therefore smaller in the near term than in the far term: deciding three hours ahead does not
carry the same uncertainty as deciding thirty-six hours ahead.

**This nuance does not affect the table above in any way.** The five factors are
systematic and independent of the horizon: shading, soiling and module temperature
are as wrong at three hours as at thirty-six. A near horizon improves
the weather, never the conversion.

This is exactly the line of §2.5: the ensemble spread measures what improves with
proximity, and stays blind to what does not.

Consequence to accept: **4 kW forecast are not 4 kW measured**, and the deviation is neither
random nor centred — it is biased high. Neither the engine nor the app may treat these
values as a measurement, nor display them without a forecast marker.

### 0.2 Forbidden to the consumer

- Feeding a real-time control loop with `forecastPvPower` (that is the job of the
  inverter, which measures).
- Displaying a `forecast*` state without visually marking that it is a forecast.
- Comparing `forecastPvPower` with a measured production to derive a correction from it:
  that computation is learning, it belongs to the proprietary scope.

---

## 1. States (openmeteo ThingClass)

Reminder from CLAUDE.md: nymea has no `WattPerSquareMeter` unit → for irradiances,
`unit` is **omitted** in the JSON and "(W/m²)" appears in the label.

### 1.1 Current instant (refreshed every hour)

This is **not** a "live" measurement: it is the current hourly step of a
forecast published up to 5 h earlier (§3.1).

| State | Thing | Type | Unit | Description |
|---|---|---|---|---|
| `forecastPvPower` | plane | double | Watt | Forecast PV production at the current step (`current` of the API, instantaneous), rounded to 50 W; temperature-corrected with `current.temperature_2m` when the run is (§2.6) |
| `forecastGtiSource` | plane | double | — (W/m²) | **Source quantity** of the line above, published for verification. Not a second forecast. |
| `cloudiness` | site | int | % | Forecast sky cover 0–100 (name imposed by the `weather` interface, §1.6) |
| `temperature` | site | double | °C | Forecast air temperature (air, **not** module; name imposed by the interface) |
| `forecastModel` | site | string | — | Model that actually answered (§3.2) |
| `forecastIssuedAt` | site | int (s Unix) | — | Initialisation of the model run (§3.3) |
| `forecastAvailableAt` | site | int (s Unix) | — | Publication of the run, informative (§3.3) |
| `updateTime` | site, plane | int (s Unix) | — | `current.time` of the reply: the instant of the API's current step, **not** the time of reception |
| `forecastValid` | site | bool | — | `false` if no successful forecast reply for 3 × 3600 s; re-evaluated at each tick |

`forecastGtiSource` and `forecastPvPower` carry the same information up to a constant
factor. The first exists as a checkpoint (direct comparison with the
API reply), not as independent data — hence the `Source` suffix. A consumer
that displays both as two forecasts is mistaken.

### 1.2 Sliding windows (cross midnight — by construction)

| State | Type | Unit | Description |
|---|---|---|---|
| `forecastEnergyNext3h` | double | kWh | Forecast PV energy over the next 3 h (sum of the hourly powers of §2.1) |
| `forecastEnergyNext6h` | double | kWh | same, 6 h |
| `forecastEnergyNext12h` | double | kWh | same, 12 h |

**These windows are strictly forward-looking and cross midnight.** This is not a
defect, it is their definition: at 23:00, 100 % of `forecastEnergyNext12h` comes from the
next morning; at 20:00, 88 % (probe of 2026-09-08). No consumer may read
these states as "what remains today" — that is the role of `forecastEnergyRestOfDay`.

The 12 h horizon is kept although useless for the day's eco spreading: deferred
battery charging ("reach 100 % two hours before the evening peak") will
need it.

### 1.3 Bounded by the solar day

| State | Type | Unit | Description |
|---|---|---|---|
| `forecastEnergyRestOfDay` | double | kWh | Forecast PV energy from now to sunset (plane) |
| `forecastEnergyTomorrow` | double | kWh | Forecast PV energy over tomorrow's day (plane) |
| `sunsetTime` | int (s Unix) | — | Sunset, `daily.sunset` of the API (site; name imposed by `daylightsensor`) |
| `forecastPeakTime` | int (s Unix) | — | Timestamp of today's hourly sample with maximal GTI, hence the **end** of the brightest hour (plane) |
| `forecastTendencyToday` | string | — | `sunny` \| `mixed` \| `overcast` \| `unknown` (site) |

### 1.4 Rounding

Powers are rounded to **50 W** and energies to **0.1 kWh**, so as not to
suggest a precision that §0.1 forbids claiming.

The energy quantum went from 0.5 to 0.1 kWh at implementation. Reason: a 3 h window
in the evening is often worth less than 1 kWh, and a 0.5 kWh step would erase there the
difference between "it is worth waiting" and "nothing is left" — precisely the
distinction that eco spreading needs. 0.1 kWh remains very coarse compared to the bias of
§0.1; that is the point.

### 1.5 Weather spread — qualifies the weather, never the production

These states live on the **PV plane**, not on the site (§1.6): the spread is computed
on the GTI, which only exists for a given orientation.

| State | Type | Unit | Description |
|---|---|---|---|
| `forecastWeatherSpreadTodayPercent` | double | % | Relative standard deviation of the daily POA energy between the members of an ensemble. **Qualifies the weather uncertainty. Does not qualify the accuracy of the forecast production.** |
| `forecastWeatherSpreadTomorrowPercent` | double | % | same, tomorrow |
| `forecastSpreadModel` | string | — | Ensemble the spread comes from (different family from `forecastModel`, §2.5) |
| `forecastWeatherSpreadTime` | int (s Unix) | — | Instant (box clock) of the last **successful** spread. 0 = never. Since `etm16` |
| `forecastWeatherSpreadValid` | bool | — | `true` as long as `forecastWeatherSpreadTime` is at most 2 × 10,800 s old; re-evaluated at each tick. Since `etm16` |

**Invalid, and how it is written** (since `etm16`). Beyond **21,600 s** (2 × the 3 h
interval) without a successful spread, or if none has ever been computed,
`forecastWeatherSpreadValid` goes to `false` and both percentages go to
**invalid**; a day that a successful reply does not allow to compute (fewer than 5 complete
members) also goes to invalid, the other one remaining published. **Never 0**: 0 % would read
"no uncertainty".

- *Value.* A nymea state cannot carry `null`: `Thing::setStateValue` converts the value
  to the declared type and **rejects** an invalid `QVariant` ("Type mismatch… Discarding change").
  The state therefore carries **NaN**, which it accepts; nymea's JSON-RPC serialises through
  `QJsonDocument`, which writes NaN as **`null`**. The client receives `"value": null`. The state
  cache (QSettings) reads NaN back (verified, Qt 6.8). The plugin writes NaN only once (NaN ≠ NaN:
  without a guard, every tick would notify).
- *Associated state.* `forecastWeatherSpreadValid` (bool) and `forecastWeatherSpreadTime` (age).
- *What the app reads.* Band for day D drawn **if and only if** `forecastWeatherSpreadValid`
  is `true` **and** the percentage for day D is a number (not `null`). Otherwise: no band,
  never a band at 0. The age (`now − forecastWeatherSpreadTime`) may be displayed in
  installer mode. Unhandled caveat: "today" is the local day of the computation; in the
  three hours after midnight, a spread computed the day before still applies to the day before
  (the app can detect it with `forecastWeatherSpreadTime`).

**These states are not a confidence, and the contract forbids presenting them as
such.** See §2.5 for what they measure and what they do not measure.

Choice of form, deliberate: a percentage quantity named "spread", and not an
enum `low` / `medium` / `high`. An enum is a judgement, and a judgement without an
object reads as "confidence in the published figure". This is the same mechanism as in §0:
the name carries the meaning, because a note at the bottom of a table gets ignored.

---

### 1.6 Two ThingClasses, and two exceptions to §0

The implementation has two ThingClasses, not one: **`openMeteoSite`** (weather of the location)
and **`openMeteoPvPlane`** (a PV plane, child of the site). The energy and spread states
live on the plane, since they depend on `tilt` / `azimuth` / `kwp`.

**Exception 1 — the nymea interfaces impose their names.** `openMeteoSite` declares the
interfaces `weather` and `daylightsensor`, which require exact state names
(`temperature`, `humidity`, `pressure`, `windSpeed`, `windDirection`,
`weatherDescription`, `weatherCondition`, `daylight`, `sunriseTime`, `sunsetTime`).
Prefixing them would break interface compliance. §0 therefore applies to everything else — the
irradiances and the energies, that is, the states the engine reads. Details and
trade-off: `MAPPING_openmeteo.md` §1.

**Exception 2 — there is a second regime, observation.** The plugin also queries
`satellite-api.open-meteo.com`, which provides a satellite-**observed** irradiance.
This is not a forecast, and the prefix says so: `observed*` versus `forecast*`.

| prefix | regime | latency |
|---|---|---|
| `forecast*` | MeteoFrance forecast | published 4–5 h after the run |
| `observed*` | satellite observation | **≈ 59 min** (measured) |

`observedTime` publishes the timestamp of the satellite sample, not that of the request:
an observation one hour old must not read as a snapshot.

The clear-sky value missing in §2.4 **exists** on the satellite product (probe: 495.8
observed for 694.5 under clear sky, kt = 0.71). Hence two distinct indices, not to be
confused: `observedClearSkyIndex` (observation, present instant) and
`forecastTendencyToday` (forecast, whole day, denominator `terrestrial_radiation`). The
satellite clear-sky value only exists for the past: it cannot qualify a forecast.

No `observedPvPower` is published, deliberately — see `MAPPING_openmeteo.md` §4.1:
that state would sit next to a production actually measured by the inverter and
would invite the comparison that §0.2 forbids.

### 1.7 `observedGtiSeries` — the hourly series of the satellite GTI (plane, since `etm16`)

`observedGti` / `observedGtiTime` only carry the last non-zero hour: an hour that became
available between two reads was lost (simulated at 0.03 % to 31 % of hours depending on the
regularity of the latency, in simulation). They are **kept as they are** — `ems` etm69 reads
them — and the whole series of the same reply is published alongside:

```json
{ "v": 1, "t0": 1791928800, "step": 3600, "unit": "W/m2",
  "convention": "mean over [t, t+step)", "regime": "observed",
  "source": "satellite_radiation_seamless",
  "gti": [0, 0, …, 824.0, …, 0, null, …, null] }
```

- `QVariantMap`, `cached: false`, never logged (same reasons as §5.1, §5.4).
- **Window**: yesterday 00:00 local → today 24:00, 48 slots, absolute instants (s Unix).
  Two days: an hour not yet observed today comes back as a slot of yesterday in the
  next day's reply.
- **Instants**: slot `i` covers `[t0 + i·step, t0 + (i+1)·step)` and reads the satellite
  sample timestamped `t0 + (i+1)·step`, **mean of the hour ending there** — the convention
  of the transport (§5.3). `targetAt = t0 + i·step`; the slot of `observedGtiTime` is therefore
  `observedGtiTime − 3600`, the rule that `ems` applies (§31.7 of the engine design).
- **Absence**: `null` (latency ≈ 1 h, hours to come, gap in the reply), never 0; night
  is a published 0. Reply without any non-zero sample: nothing is published, and it counts as a
  failure for the retry (§3.1).

### 1.8 `forecastHourlyWeather` — hourly site weather, for the app (site, since `etm16`)

Weather icon and temperature per hour for the app's « Journée » card. On the **site**
only: these quantities depend on the coordinates and the altitude, not on an orientation.
**No new request and no new host**: `weather_code,is_day,temperature_2m` are added to the
`hourly=` of the site's forecast request (§3.4), which already sends coordinates and altitude.

```json
{ "v": 1, "t0": 1792015200, "step": 3600, "regime": "forecast",
  "convention": { "weatherCode": "WMO code, instantaneous at t",
                  "isDay": "1 day, 0 night, instantaneous at t",
                  "tempC": "temperature_2m in C, instantaneous at t" },
  "model": "meteofrance_seamless", "issuedAt": 1792036800,
  "weatherCode": [3, 3, …, 0, 0, …], "isDay": [0, 0, …, 1, …], "tempC": [8.3, 8.0, …] }
```

- `QVariantMap`, `cached: false`. Window: **today 00:00 local → tomorrow 24:00**, 48 slots,
  exactly the `forecast_days=2` horizon already requested. `t0` is an absolute instant; the app
  converts to local time **at display**.
- `model`: the model that **answered** (`best_match` fallback included, §3.2); `issuedAt`: as for
  the curve (§3.3, 0 = unknown).
- **Instants — verified, not assumed.** The three variables are **instantaneous** at their
  timestamp: Open-Meteo documentation (`/en/docs`, read on 2026-10-15: `temperature_2m`,
  `weather_code` and `is_day` "Instant"); **measured** for `temperature_2m` (the hourly value at
  `T` equals the `minutely_15` value at `T` for 45 hours out of 47, the mean of the previous hour
  for 1 — test `temperatureIsInstantaneousOnARealReply`) and for `is_day` (sunrise 07:48 → 0 at
  07:00, 1 at 08:00; sunset 18:39 → 1 at 18:00, 0 at 19:00: only the hypothesis "instantaneous at
  `T`" yields both edges). **Not verified**: the precipitation part of `weather_code` — the
  server derives it from accumulations that cover the previous hour, and the day of measurement
  was dry.
- **Published convention**: slot `i` covers `[t0 + i·step, t0 + (i+1)·step)` like the transport,
  and carries the value **at its start** `t0 + i·step` (= `targetAt`). Accepted difference with
  radiation, whose sample `T` averages the previous hour and which therefore reads
  `t0 + (i+1)·step` (§5.3). Consequence: the `tempC` of this state at slot `i` is the `tempC` of
  the transport at slot `i − 1` (same model, same altitude since `etm16`). A slot that contains
  sunset carries the `isDay` of its start: an app that wants better has `sunriseTime` /
  `sunsetTime`.
- **Absent = `null`, never 0 — critical**: WMO code **0 = clear sky**, `isDay` 0 = night.
  A variable absent from the reply, or of a different length than `time`, comes out as 48 `null`
  (tested). None of the three: nothing is published (warning in the log; the radiation
  forecast, for its part, remains a success).
- **Why a series in a state** (CLAUDE.md: "scalar by default; a series is allowed if it comes
  out of a deterministic model"): these 48 × 3 values are the output, relayed without
  transformation, of a numerical weather prediction model (deterministic, no learning on the
  site's data) — the same status as `forecastCurveTransport`. Splitting it into 144 scalar
  states would break the only useful guarantee, that they all come from the same reply. Same
  reasoning for `observedGtiSeries` (§1.7), produced by a deterministic satellite retrieval
  algorithm. The app reads these two states; it does not read `forecastCurveTransport`,
  an internal format towards `ems`.

---

## 2. Allowed computations

### 2.1 Power and energy

    P(GTI, T)       [W]   = GTI × kwp × performanceRatio × f(GTI, T)     (f of §2.6, or 1)
    forecastPvPower [W]   = P(GTI current, temperature_2m current), rounded to 50 W
    forecastEnergy  [kWh] = Σ round(P(GTI hourly, temperature_2m hourly)) / 1000

**Since `etm16`**, the scalar states follow the **run decision**, that of the curve:
`f` is the temperature factor of §2.6 if and only if the curve of the same cycle is
corrected (`corrections: ["temperature"]`: complete temperature over the 48 slots), otherwise
`f = 1` and everything is rule of three. The energies sum **the same rounded hourly powers
as `p`**: a window is worth the sum of the `p` values published over it, to within the 0.1 kWh
rounding. `forecastPvPower` takes `current.temperature_2m`; if it is missing on a corrected run,
the state **is not updated** rather than published uncorrected next to corrected energies.
Before `etm16`, the scalars stayed in rule of three and their sum deviated from that of
`p` (−3.06 % over the fixture day of 2026-10-14; estimated order of magnitude, not measured,
−10 to −12 % at peak hours of a clear summer day).

`kwp` is declared by the installer (default 0, no bound: a plane left unset publishes
zeros). No other factor enters these scalar states: no default PR,
no derating. The `performanceRatio` parameter
exists (§4) and is **1.00** by default, precisely so that the published figure is
a bare linear transformation of the API value, with no hidden coefficient. An
installer who sets it below 1 declares a derating; they do not correct a
measurement.

### 2.2 Summation (indexing trap)

`global_tilted_irradiance` is the **mean of the hour preceding** the timestamp. A
window of N hours from `now` therefore sums the steps `t+1 … t+N`, **not**
`t … t+N-1`.

**This convention is not an assumption, it is measured** — but by two
distinct checks, because neither of them is sufficient alone.

**General verification rule for this plugin**, to be followed in tests as in
any later investigation: an identity whose two terms come from the same
source verifies nothing — it confirms that the code is consistent with itself, which nobody
doubted. Only an identity one of whose terms is produced outside our control is
a proof.

#### Check 1 — integration and day bounds

The hourly sum aggregated over `J 01:00 → J+1 00:00` reproduces the
`daily.shortwave_radiation_sum` computed by Open-Meteo to within **+0.05 % / −0.03 % / +0.02 %**
over three days (2026-09-08). This establishes that the hourly values are true
hourly means and that our day bounds are right.

**This check does not distinguish backward-looking from forward-looking**, and a first draft of
this contract wrongly claimed it did. The day boundary falls at midnight, where irradiance is
zero: shifting the summation there by one hour changes no total. Verified by deliberately
breaking the convention — check 1 keeps passing.

#### Check 2 — the backward-looking convention itself (discriminating)

`shortwave_radiation` (mean) versus `shortwave_radiation_instant` (instantaneous), two
series that Open-Meteo publishes independently of us. If the mean at HH covers
`(HH−1, HH]`, it must follow the midpoint of `instant[HH−1]` and `instant[HH]`; if it
covered `(HH, HH+1]`, it would follow the next midpoint.

| hypothesis | mean absolute error |
|---|---|
| backward-looking | **26.0 W/m²** |
| forward-looking | 83.2 W/m² |

Ratio 0.31 over 37 samples (2026-09-08). It is this check that proves the convention.

#### Form required for the non-regression test

`openmeteo/tests/check_integration_convention.py` replays **both**, separately, and
never the plugin's code against itself. Both have been validated by a negative check:
broken, they fail.

### 2.3 Day bounds

`forecastEnergyRestOfDay` is bounded by `daily.sunset`, not by `is_day`. `is_day` is
an instantaneous hourly flag: it is 0 at 07:00 for a sunrise at 07:17, which would lose
the 43 min of daylight of the 07:00→08:00 bucket. `is_day` serves to qualify an hour, never to
integrate.

### 2.4 Trend

`forecastTendencyToday` = clearness index, thresholds on `Σ GHI / Σ terrestrial_radiation`
over the local day: > 0.55 = `sunny`, 0.25–0.55 = `mixed`, < 0.25 = `overcast`;
`unknown` if `Σ terrestrial_radiation` is zero.

**Correction relative to v1 of this contract**, which planned a ratio to Open-Meteo's
clear-sky value: `shortwave_radiation_clear_sky` returns **0 non-zero values** on the
MeteoFrance models, and `global_tilted_irradiance_clear_sky` does not exist.
`terrestrial_radiation` (extraterrestrial, astronomical) is the only reference
available over the whole horizon.

Boundary caveat: `tendency` remains a **descriptive label**. If a consumer
starts deriving a decision weighting from it, the logic moves to the
`etm-powersync-optimizer` side.

---

### 2.5 Spread: what it measures, what it does not measure

Source: `https://ensemble-api.open-meteo.com/v1/ensemble`, accessible **without a key**,
and which exposes `global_tilted_irradiance` per member with `tilt`/`azimuth` — so the
spread does apply to the published quantity, not to horizontal radiation.

    forecastWeatherSpread%  = 100 × stddev( Σ GTI_member over the day )
                                  / mean( Σ GTI_member over the day )

**On the GTI, not on the GHI, and this is measured.** A computation on the horizontal GHI would
have allowed carrying the spread on the site rather than on each plane. Probe of
2026-09-08:

| day | CV(GHI) | CV(GTI) | difference |
|---|---|---|---|
| 09-08 | 10.0 % | 11.9 % | 2.0 pt |
| 09-09 | 10.1 % | 12.0 % | 1.9 pt |
| 09-10 | 7.8 % | 10.3 % | 2.5 pt |
| 09-11 | 33.8 % | 38.4 % | 4.6 pt |

CV(GTI) systematically exceeds CV(GHI), and the difference grows with the spread. The two
are not interchangeable, and the approximation would go in the optimistic direction —
underestimating the spread of the published quantity. Hence the per-plane computation.

**No MeteoFrance ensemble exists** (`meteofrance_arpege_ensemble`,
`..._probabilities`, `..._arome_france_ensemble`: all rejected by the endpoint).
The spread therefore necessarily comes from **another model family** than the
published forecast. Chosen model: `icon_seamless` (40 members, hourly GTI, 4-day
horizon). Hence the obligation to publish `forecastSpreadModel`: §3.2 requires stating
which model answered, and the rule also applies to this one.

Consistency check performed (2026-09-08, one site, 4 days): the published MF forecast
falls within the ICON envelope on all 4 days, at percentiles 15 / 41 / 59 / 54. This is a
sample too short to validate the cross-qualification; it only suffices to
observe that it is not absurd. **To be re-examined if the deviation becomes observable.**

Measured amplitude: CV of 10–12 % at D+0…D+2, **38 % at D+3** (members from 0.90 to
6.41 kWh/m² on the same day). The quantity really discriminates.

#### What it does not measure — main restriction

The ensemble spread measures the **meteorological uncertainty**, that is, a
random and centred error. The five factors of §0.1 — shading, soiling,
module temperature, clipping, dirt — are **systematic and all optimistic**.
The spread sees none of them.

Direct consequence, and the reason this subsection exists: under a stable clear sky, the
spread will be **low** at the very moment when the conversion bias is **maximal**
(the thermal efficiency drop of the modules peaks in clear and hot weather).
The two quantities are anticorrelated. A consumer who read "low spread"
as "reliable figure" would certify a wrong value at the worst moment — exactly the
defect that §0 exists to prevent.

#### Forbidden to the consumer (complements §0.2)

- Presenting `forecastWeatherSpreadPercent` as a confidence, a reliability, a
  precision or an error margin on `forecastPvPower`.
- Deriving from it a confidence interval on an energy in kWh: the spread does not
  cover the conversion bias, the interval would be wrong and too narrow.
- Using it to weight an arbitration decision. Weighting by uncertainty
  is a strategy: it belongs to `etm-powersync-optimizer` (CLAUDE.md).

Legitimate use: telling the user that tomorrow's **weather** is uncertain, and
letting an optimizer that knows what it is doing draw whatever it wants from it.

#### Cadence and cost

Refresh every **3 h** (ensemble runs come out every 6 h;
`ecmwf_ifs025_ensemble` probed at 6 h / 3 h resolution). An ensemble request carries
40 variables: Open-Meteo pricing counts requests with more than 10 variables
as several calls — estimate ~4 calls, **not measured** (the API returns no
quota header). That is ~32 calls/day, with no effect on the 10,000 cap.

Ensemble failure: the values are not rewritten, the retry follows §3.1, and
**beyond 21,600 s without success** the percentages go to invalid (NaN, `null` in
JSON) with `forecastWeatherSpreadValid = false` (§1.5, since `etm16`). *(Up to `etm15`,
the last value stayed published without an age — frozen for two days on a test installation —
and the failed request was retried every minute.)*
**The rest of the contract does not depend on it** — the spread is an extra, never a
validity condition of the forecast.

---

### 2.6 Module temperature correction — curve and, since `etm16`, scalar states

Decision of 2026-10-12 (`DESIGN_PREVISION_PV.md` §28.1, engine repository): the physical
parameters **per array** live on the plane Thing, next to `tilt`, `azimuth`, `kwp` and
`performanceRatio`, and it is **this plugin** that applies the correction. A single source of
truth for the physics of an array.

For each hourly slot `i` of the curve (§5):

    Tcell = T2m + GTI × (noctC − 20) / 800
    p     = max(0, GTI × kwp × performanceRatio × (1 + tempCoeffPctPerK / 100 × (Tcell − 25)))

`tempCoeffPctPerK` and `noctC` are **declared** (module datasheet), never learned;
defaults −0.37 %/K and 45 °C, which existing planes take without any action. Reference
values (tested): GTI 1000 W/m², 25 °C → Tcell 56.25 °C, factor 0.884375; GTI 800,
−5 °C → Tcell 20 °C, factor 1.0185 — **a gain in the cold**, published as is.

- **All or nothing per run.** A single slot without temperature in the 48-slot window, or a
  `temperature_2m` series of a different length than `time`: the whole run is published **without
  correction**, `corrections: []`, and `p` is then **bit-for-bit identical** to v1. Never
  a mixed curve: its list of corrections could not name it.
- **Pairing by instant.** `GTI` is the mean of `(T−1 h, T]`, `temperature_2m` the
  **instantaneous** value at `T`; slot `i` takes both at `T = t0 + (i+1)·step`. The offset
  from the middle of the hour (half an hour of thermal drift, a few tenths of a kelvin)
  is accepted, and not interpolated.
- **No AC cap here**: clipping applies to what the inverter receives from all its
  arrays, it is a site parameter applied by `ems` after the sum. A plane that
  announced `ac-limit` is refused by `ems`.
- The **scalar states** (`forecastPvPower`, `forecastEnergy*`) follow the same decision
  since `etm16` (§2.1): corrected along with the curve, or not at all. Up to `etm15` they
  stayed in rule of three.

This is not learning: no term is derived from a forecast/measured comparison.
§0.2 and §7 remain fully in force.

---

## 3. Network behaviour

### 3.1 Cadence

**1 h**, with random jitter ±60 s — that is **59 to 61 min**, not "about one hour".

The `PluginTimer` ticks every **minute**. It used to tick every quarter of an hour, and one night
of measurement on a test installation showed what that produced: cycles alternating between 60
and 75 min, **67 min on average** instead of the announced 60.

The real defect was not the average but the jitter: quantized to the quarter hour, it only chose
*which* tick to take, never *when* within the tick. A whole fleet would have stayed aligned on the
same 15-minute grid — whereas desynchronizing the fleet is its only reason to exist. A jitter
needs a finer resolution than the spread it produces.

The cost of the one-minute tick is one hash lookup and one comparison per Thing per minute.
Querying more often would only re-read the same data: MeteoFrance runs come out every 3 h (AROME)
to 6 h (ARPEGE), and are published **4 to 5 h after their initialisation** (run 06Z probed as
available at 11:08 UTC).

The jitter is drawn **once**, when the next attempt is scheduled (since `etm16`; before, it was
redrawn at every tick, which on average brought the request forward).

**API failure — retry with doubled delay** (since `etm16`). After a failure, the next attempt
starts **5 min × 2^(n−1)** after the failure, `n` being the number of consecutive failures,
**capped at the source's interval**, jitter ±60 s; `n` goes back to 0 at the first success.

| source | interval | delays after 1, 2, 3… failures |
|---|---|---|
| forecast pass (site + its planes) | 1 h | 5, 10, 20, 40, 60, 60… min |
| satellite (site; each plane) | 1 h | 5, 10, 20, 40, 60… min |
| ensemble (each plane) | 3 h | 5, 10, 20, 40, 80, 160, 180… min |

- **Counts as a failure**: network or HTTP error, unreadable body, `error` field from the API, and
  any **200 response that carries nothing** — model without radiation after fallback (§3.2),
  series missing or of the wrong length, no non-zero satellite sample, no ensemble member or no
  computable spread, incomplete 48 h curve.
- **An attempt counts as a failure as long as its response has not arrived**: a hanging request
  does not block the source, and does not make it ask again every minute.
- The **forecast pass** of a site (its request and those of all its planes) is a single source:
  it succeeds if all succeed, and a failure of one retries them all. Retrying one plane alone
  could make it take a more recent run than its neighbours and would break §5.7. A plane in
  permanent error therefore brings the site back to the hourly cadence, not below.
- A forced refresh (manual action, setup) overrides the delay.
- Originally observed in testing: an ensemble that answered 500 was asked again every minute
  (1,440 requests per day); with the retry, 13 over a day of continuous failure (test
  `aFailingSourceIsNoLongerAskedEveryMinute`). *(In v1 this paragraph announced "retry ×2 (10 s,
  60 s)", never written; realigned on 2026-10-13, fixed in the code in `etm16`.)*

### 3.2 Model and fallback — the fallback must say so

Nominal model: `meteofrance_seamless` (AROME 1.3 km over 60 h, then ARPEGE; GTI available over
111 h ≈ D+4.5).

If the nominal model returns radiation that is entirely `null` over the requested horizon — site
outside the domain, or variable unavailable as on `meteofrance_arome_france_hd`, which exposes
**no** radiation at all — the plugin replays the request on `best_match`, once only.

**The defect cannot be read in the return code.** A model that does not serve the variable
answers **HTTP 200 without an `error` field**, with `null`s. So it is the response that must be
inspected, not the status. Verified on a test installation by configuring
`meteofrance_arome_france_hd`: the fallback triggers, the log says so, and `forecastModel`
switches to `best_match`.

**This fallback is not silent.** `forecastModel` is only written from the request that actually
served the data — it is a provenance, not an intention. MeteoFrance without GTI is not a degraded
version of the same service, it is a service that cannot do it: `best_match` blends other models
(ECMWF, ICON) at another resolution. Without publishing the model, the announced accuracy no
longer matches the accuracy obtained, and nobody can know it. This is the rule of the regime
(§0), applied to the source.

If both fail: nothing is written, and `forecastValid` switches to `false` at the latest 3 h after
the last successful response (§1.1). No invented value — but the states keep their last value
until then.

### 3.3 Run provenance — and what we do when it is missing

Two timestamps, read from `{baseUrl}/data/<model>/static/meta.json` **for the model that actually
answered** (§3.2):

| state | meta.json field | role |
|---|---|---|
| `forecastIssuedAt` | `last_run_initialisation_time` | **revision key** |
| `forecastAvailableAt` | `last_run_availability_time` | informative |

**Never confuse the two.** The initialisation says when the run describes the world; the
availability, when it became readable. Gap measured on 2026-09-08:

| model | gap |
|---|---|
| AROME France HD | 4.28 h |
| ARPEGE Europe | 4.35 h |
| ECMWF IFS 0.25° | 7.12 h |
| ECMWF ensemble | 8.87 h |

Four to nine hours, and **the gap varies with the model**: no flat offset would make up for it.
Dating a revision by availability would attribute it to a run that did not exist yet, and both
numbers are equally plausible — nobody would see it.

**`forecastIssuedAt` is the identity of the run**, the one that answers "what were we forecasting
at 8 am". `forecastAvailableAt` says since when the data was readable; it must **never** be used
as a key.

#### Composite names have no `meta.json`

Only **real grid domains** serve `meta.json`. Measured on 2026-09-08: `meteofrance_seamless`,
`best_match`, `icon_seamless`, `icon_eu`, `meteofrance_arome_france` all answer **HTTP 500**. A
composite name is not a domain: it is a blend resolved by the server.

The plugin therefore translates the model into a provenance domain:

| queried model | provenance domain | reason |
|---|---|---|
| `meteofrance_seamless` | `meteofrance_arome_france_hd` | both AROME grids (0.01° and 0.025°) come from the **same run**, and it is the AROME run that carries the first 60 hours |
| `best_match` | *(none)* | resolved per site, no single run identity |
| any real domain | itself | |

> ⚠️ **A more serious limitation than the approximation first announced.** This contract
> justified the translation by "both AROME grids come from the same run". That is true of the
> run identity, but insufficient: **`meteofrance_arome_france_hd` serves no radiation value at
> all** — 0 out of 72 steps, measured on 2026-09-09, as already noted in §3.2. The domain that
> `issuedAt` comes from **therefore contributes nothing to the curve**.
>
> And the domain that actually provides the radiation (`meteofrance_arome_france`, or the blend
> the server composes over the first hours) **exposes no `meta.json`** — HTTP 500, measured.
> There is therefore *no* run identity available for the data that forms the curve.
>
> **The `seamless` merge rule, measured** (2026-09-09). It is not documented by Open-Meteo — the
> docs only say "Seamless combines all models from a given provider". Derived by direct
> comparison:
>
>     seamless = w · arome_france_15min + (1 − w) · arome_france
>
> | hour | 15 min | hourly | seamless | w |
> |---|---|---|---|---|
> | 08:00 → 10:00 | 33.4 · 142.5 · 144.2 | 29.2 · 153.5 · 356.0 | = 15 min | **1.00** |
> | 11:00 | 257.5 | 530.3 | 325.7 | **0.75** |
> | 12:00 | 380.5 | 490.4 | 435.5 | **0.50** |
> | 13:00 | 259.3 | 325.6 | 309.0 | **0.25** |
> | 14:00 and beyond | *(horizon exceeded)* | — | = hourly | **0** |
>
> Linear ramp over the last three hours of the horizon of the 15-minute domain, which stopped at
> 13:00, i.e. 5.2 h ahead.
>
> **This is not a satellite nowcast**: the `satellite_radiation_seamless` product only serves the
> past (verified), and the first hours *exactly* equal `meteofrance_arome_france_15min`. The near
> hours are therefore not corrected by observation — it is the **same model, initialised more
> recently**.
>
> **Consequence to keep in mind: age is local to the slot, not global to the curve.** The
> 15-minute domain refreshes **every hour** (read on its twin `arome_france_hd_15min`,
> `update_interval_seconds = 3600`), the hourly one every 3 h. The first hours of the curve are
> therefore fresher than the following ones — and that is the only reason why they move under a
> stable `issuedAt`.
>
> **`issuedAt` is an approximate age indicator, borrowed from a neighbouring domain. It is not
> the identity of the content, and §5.6 draws the consequences.** It remains useful — it says
> *roughly* when the forecast dates from — and it remains the right key to answer "what were we
> forecasting this morning". It cannot be used as a uniqueness key.

When `best_match` is active — so after a fallback, an already degraded situation — the
provenance is **really** unknown and is 0. The consumer then does not record a revision, which
is the right answer: we do not know which run the data comes from.

#### Cost: one conditional request

`meta.json` carries an `ETag` whose first field is the publication timestamp —
`"1788873393-591"`. The plugin keeps the ETag per model and sends an `If-None-Match`: **304 and
0 bytes** as long as the run has not changed, 200 when it changes. The ETag is therefore itself
the new-run signal. The request goes to `/data/`, not to `/v1/forecast`.

#### When `meta.json` does not answer — decided, not discovered

`forecastIssuedAt` and `forecastAvailableAt` switch to **0**. The forecast itself remains
published and valid: only the key is missing.

**Consumer obligation**: `forecastIssuedAt == 0` means *unknown age*. It is explicitly forbidden
to substitute the reception time or `updateTime` for it — the real gap is 4 to 9 hours, so the
value would be wrong in a plausible and undetectable way. **Store the age as `NULL`, and nothing
else.**

*Wording corrected on 2026-09-09.* This clause said "do not record the revision". That was
right as long as `issuedAt` served as the key; it no longer is since §5.6: the curve remains
perfectly usable, only its age label is missing, and throwing away valid data for an absent
piece of metadata would be a dead loss. The prohibition is on **substituting an instant**, not
on keeping the curve.

An absent age is a visible gap; an invented age is an invisible lie. Silence remains the correct
behaviour of the instrument that does not know — but it keeps quiet about what it does not know,
not about what it knows.

### 3.4 Request

*Realigned on the code on 2026-10-13*: the previous version described a single request that
does not exist. Parameters common to all: `latitude`, `longitude` (rounded to 2 decimals ≈ 1 km),
`timezone=Europe/Paris`, `timeformat=unixtime`, and `apikey` if the parameter is set.

    Site — GET {baseUrl}/v1/forecast                     every hour (§3.1)
        models=<weatherModel>          (default meteofrance_seamless; best_match on fallback)
        elevation=<m>                  only if the site parameter is non-zero
        wind_speed_unit=ms
        current=temperature_2m,wind_speed_10m,wind_direction_10m,cloud_cover,
                relative_humidity_2m,surface_pressure,weather_code,snowfall,is_day,
                shortwave_radiation,direct_normal_irradiance,diffuse_radiation,
                terrestrial_radiation
        hourly=shortwave_radiation,terrestrial_radiation,
               weather_code,is_day,temperature_2m        (the last three since etm16, §1.8)
        daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,shortwave_radiation_sum
        forecast_days=2

    Plane — GET {baseUrl}/v1/forecast                    within the site's pass (§5.7)
        models=<weatherModel>, tilt, azimuth (§3.5)
        elevation=<site m>             same rule as the site (since etm16)
        current=global_tilted_irradiance,temperature_2m  (temperature_2m since etm16, §2.1)
        hourly=global_tilted_irradiance,temperature_2m   (temperature_2m since etm15, §2.6)
        forecast_days=3

    Site — GET {satelliteBaseUrl}/v1/archive             every hour
        models=satellite_radiation_seamless
        hourly=shortwave_radiation,shortwave_radiation_clear_sky
        past_days=1, forecast_days=1
    Plane — GET {satelliteBaseUrl}/v1/archive            every hour
        models=satellite_radiation_seamless, tilt, azimuth
        hourly=global_tilted_irradiance, past_days=1, forecast_days=1

    Plane — GET {ensembleBaseUrl}/v1/ensemble            every 3 h
        models=<ensembleModel> (default icon_seamless), tilt, azimuth
        hourly=global_tilted_irradiance, forecast_days=3

    GET {baseUrl}/data/<domain>/static/meta.json         provenance (§3.3), conditional

Site data sent: rounded coordinates, **elevation** (if set, on both forecast requests) and plane
orientation (`tilt`, `azimuth`). Nothing else.

**Elevation on the plane request (`etm16`).** Without it, the `temperature_2m` that corrects the
curve was at the elevation of the model's terrain (156 m at the default point), not at the
declared one. Satellite and ensemble do not carry it: they only serve irradiance. Measured effect
(two responses from the same minute, test plane, 153 m declared): GTI identical over the 72
steps, `tempC` +0.1 K on 1 cell out of 48, `p` unchanged over the 48 cells. General order of
magnitude: gradient ≈ 0.65 K / 100 m, i.e. ≈ 0.24 % of `p` per 100 m of difference between
declared elevation and model terrain (coefficient −0.37 %/K). Instants are Unix seconds
(absolute); `timezone` only serves the API to bound the days and return `utc_offset_seconds`,
which the plugin reads for its own day bounds.

### 3.5 Azimuth conversion

    azimuth_openmeteo = azimuth_geo - 180

Verified empirically (vertical plane): `azimuth=-90` peaks in the morning (east), `0` at south,
`+90` lingers until 20:00 (west). Strict API bounds, HTTP rejection out of range without silent
clamping: `tilt` ∈ [0, 90], `azimuth` ∈ [−180, 180]. The plugin sticks to them before sending:
`tilt` is bounded [0, 90] by the parameter declaration, and the converted azimuth is brought back
into [−180, 180].

---

## 4. Thing configuration (installer)

*Realigned on `integrationpluginopenmeteo.json` on 2026-10-13* (names, types and defaults of the
previous version did not match the code).

| Parameter | Carried by | Type | Default | Note |
|---|---|---|---|---|
| `latitude` / `longitude` | site | double | 999 / 999 | **must be entered**: the default is an out-of-bounds sentinel, refused (below); latitude ∈ [−90, 90], longitude ∈ [−180, 180], bounds included; rounded to 2 decimals when sent |
| `elevation` | site | double (m) | 0 | 0 = not sent; otherwise sent with the site's forecast request and, since `etm16`, with that of each plane |
| `tilt` | plane | double (°) | 30 | panel tilt, bounded 0–90 |
| `azimuth` | plane | double (°) | 180 | **geographic convention** (180 = south), bounded 0–360, converted when sent (§3.5) |
| `kwp` | plane | double | 0 | installed peak power, declared, unbounded |
| `performanceRatio` | plane | double | 1.00 | declared derating, never learned (§2.1), bounded 0.1–1 |
| `tempCoeffPctPerK` | plane | double (%/K) | −0.37 | module power coefficient, declared (§2.6), bounded −1–0 |
| `noctC` | plane | double (°C) | 45 | module NOCT, declared (§2.6), bounded 30–70 |
| `baseUrl` | plugin | string | `https://api.open-meteo.com` | self-hosting possible |
| `satelliteBaseUrl` | plugin | string | `https://satellite-api.open-meteo.com` | |
| `ensembleBaseUrl` | plugin | string | `https://ensemble-api.open-meteo.com` | |
| `weatherModel` | plugin | string | `meteofrance_seamless` | |
| `ensembleModel` | plugin | string | `icon_seamless` | |
| `apiKey` | plugin | string | empty | if set, sent as `apikey` (§6) |

The time zone is not a parameter: `Europe/Paris` is written in the code.

**A site without entered coordinates is not set up.** Coordinates absent, equal to the sentinel
999 or out of bounds: `setupThing` fails with `ThingErrorInvalidParameter` and a message for the
installer ("Enter the site coordinates…"), and no request is sent. The planes of such a site are
not set up either (`ThingErrorHardwareNotAvailable`, "Enter the coordinates of the parent site
first.") and send nothing. The check is in the plugin (`sitecoordinates.cpp`, tested by
`tst_sitecoordinates`), not in declared `minValue`/`maxValue`: nymea would then refuse the
sentinel itself, before `setupThing`, without a message. 0 / 0 is a real point and remains
accepted. An existing site keeps its stored coordinates: nymea only takes the default for a key
absent from `things.conf`.

v1 correction: the `azimuth` label announced "Open-Meteo convention". That was wrong — Open-Meteo's
south is 0, not 180. The default value was right, the label was misleading.

---

## 5. Curve transport to ems

### 5.1 `forecastCurveTransport` — a channel, not a value

State of the `openMeteoPvPlane` ThingClass, type **`QVariantMap`**, `cached: false`, not logged.
It is the only state of the plugin that carries a structure: the name says so, because a flag can
be ignored and a name less so.

**Its shape may change with the plugin version.** A consumer reads `v` and refuses a version it
does not know, rather than parsing optimistically.

```json
{ "v": 2,
  "t0": 1791842400, "step": 3600, "unit": "W",
  "convention": "mean over [t, t+step)",
  "regime": "forecast", "corrections": ["temperature"],
  "model": "meteofrance_seamless", "issuedAt": <run>,
  "p":     [0, 0, …, 3631, …],
  "gti":   [0, 0, …, 709.4, …],
  "tempC": [6.5, 5.9, …, 16.9, …] }
```

*(Values from `openmeteo/tests/fixtures/plane-meteofrance_seamless-2026-10-13.json`, 6 kWp plane,
PR 0.9, temperature defaults; the 3631 W cell is 3831 W in v1.)*

v1 shape (up to `1.15.2+etm14`): same fields without `gti` or `tempC`, `"v": 1` and
`"corrections": "none"`. Details of v2 in §5.9.

#### Rendering in nymea-app

Verified rather than assumed. `StateDelegate.qml` routes **every non-writable state** to
`stringComponent`, which is a `Label { text: root.param.value; elide: ElideRight }`. And
assigning a map to a `QString` property fails in QML — tested:
`Unable to assign QJSValue to QString`.

Consequence: the app shows **one line with the state's label and an empty value**, plus a QML
warning in its own log. Not 48 integers poured into a list, no heavy widget: the template is an
elided line, structurally unable to become forty-eight.

It is ugly but inert, and the remedy is **on the app side**: do not display this state, as befits
the channel it is. Nothing to change here for that.

Why a typed map and not a JSON string: it is **declared in the ThingClass**, hence in the
contract, without a second encoding layer inside. JSON-RPC carries it natively — ems reads a
structure, not a string to parse — and a probe that reads the state understands what it sees.
Precedent of a non-scalar type in the nymea core: `libnymea/interfaces/useraccesscontrol.json`
declares a `QStringList`.

### 5.2 Window: whole days, past included

**Today 00:00 local → D+1 23:00, 48 slots**, elapsed hours included.

Two reasons, the second of which is decisive:

1. Comparing the forecast with today's actual production requires the morning hours, already
   past.
2. **Fixed day bounds are what make two revisions comparable slot by slot.** A sliding window
   "upcoming only" would shrink over the day: two revisions would have neither the same length
   nor the same origin, and "forecast for 3 pm seen at 8 am" would not line up with "seen at
   2 pm". That is precisely the comparison we want to make possible.

### 5.3 Indexing — stated in the payload, not left to be rediscovered

`p[i]` is the mean power over `[t0 + i·step, t0 + (i+1)·step)`. Since the API value timestamped
`T` is the mean over `(T−3600, T]`, slot `i` reads sample `t0 + (i+1)·3600`.

It is the same one-hour shift as in §2.2, and the `convention` field carries it explicitly so
that a consumer does not have to rediscover it — nor rely on the present document.

### 5.4 Publication: only when it changes

`Thing::setStateValue` **drops a write whose value is unchanged** (*"Discarding state change
[…] as the value did not actually change"*). Rewriting an identical curve is therefore neither
stored nor notified: within a single run, deduplication is structural and free.

**Exact scope, and its limit.** Deduplication applies to the **whole** payload, of which
`issuedAt` is part. A new run whose `p` are identical therefore produces a *different* payload
and **will be republished**. That is not a revision: the forecast has not changed.

The plugin cannot decide this case — it does not know whether the consumer wants to count runs
or forecasts. **It is up to the consumer to deduplicate on the `p` alone and to update
`issuedAt` on the existing row.** A revision exists only if the forecast has changed; otherwise
the table doubles for information nobody reads.

The plugin's deduplication remains useful for what it catches — repeated cycles within a single
run, which are the majority — but it is not the last line of defence.

`cached: false` because a curve reloaded at startup would be stale and would misdate the first
revision.

Finally, this state is **never logged**: neither `suggestLogging`, nor `qCDebug` of its content.
A series in the log engine would be a second, clandestine series, and a cluttered log is a log
people stop reading.

### 5.5 Embedded provenance

`model` and `issuedAt` travel **inside** the payload: a stored revision carries its own
provenance without a second call. `issuedAt == 0` means unknown age → **store the age as
`NULL`**, keep the curve (§3.3, wording corrected on 2026-09-09; this paragraph still said "do
not record the revision", realigned on 2026-10-13). It remains forbidden to substitute the
reception time for it.

`regime` and `corrections` are mandatory: a consumer that does not read this document must find
in the data that it is a forecast, and **what has been corrected** (§5.9).

### 5.6 `issuedAt` does not identify the content

A question asked by ems, and the answer decides its table key: **no**. `issuedAt` is an
**approximate provenance**, not a content fingerprint. Four mechanisms, all in the code:

**1. `t0` is anchored on the local day of the *request*, not on the run.**

    const uint todayStart = localDayStart(now, utcOffset);   // now = current.time

At local midnight, the same run produces a window shifted by one day, hence 48 different values.
Runs are 3 h apart: midnight necessarily falls within a run. **Guaranteed once a day.**

**2. `issuedAt` comes from a domain that provides no data to the curve.** This is the main
mechanism, and it is stronger than the others: `meteofrance_arome_france_hd` serves **0 radiation
values out of 72** (measured), and the domain that actually serves them exposes **no
`meta.json`** (§3.3). So nothing links `p` to `issuedAt`.

Supporting measurement (2026-09-09), on **upcoming** hours: `meteofrance_seamless` diverges from
`meteofrance_arome_france` from 08:00 to 13:00 (144.2 versus 356.0; 325.7 versus 530.3) and
coincides from 14:00 on. The first hours are recomposed by the server other than from the AROME
run alone, so they move **without `issuedAt` moving**.

**3. `best_match` gives `issuedAt = 0`** (§3.3), for all curves. Same key, arbitrary contents,
by construction.

**4. The content also depends on the plane.** `tilt`, `azimuth`, `kwp`, `performanceRatio` go
into `p`, and two planes of the same site publish two different curves under the **same**
`issuedAt`. No provenance component can tell them apart.

**5. The plugin recomposes the curve at every cycle, unconditionally.** Each cycle sends a fresh
`/v1/forecast` request and rebuilds the 48 slots from the current response. Nothing caches `p`
according to the run. Values that move under a stable `issuedAt` are therefore **not** a plugin
defect: it is the expected behaviour.

#### What the plugin does guarantee

Since `1.15.2+etm12`: **the payload is consistent with itself**. `p`, `t0`, `model` and
`issuedAt` all come from the same cycle. That is all, and it is the only thing to rely on.

#### Prohibition, for the case that has just come up

**Do not refuse a rewrite on the grounds that `issuedAt` is unchanged.** Under a stable
`issuedAt`, the content legitimately moves (points 2 and 5). Freezing the curve on the values of
the first run would display a stale forecast **without saying so** — an instrument that keeps
showing an old measurement, which is worse than one that keeps quiet.

#### Consequence for the consumer's key

A provenance key alone makes different contents collide. A **content component** is needed — and
the identity of the plane:

    key = (plane thingId, t0, fingerprint of p)
    issuedAt, model = attributes, never the key

It is the same defect as the one with multiple planes: two distinct rows overwrite each other
under a key that is too short, and `INSERT OR REPLACE` keeps one at random.

### 5.7 Consistency guarantee between planes of the same site

**All planes of a site are refreshed in the same pass, at the same instant.** The site drives the
hourly forecast of its planes; they no longer have their own scheduling for this source (the
satellite and the ensemble keep theirs, they do not need to be consistent).

This guarantee is **new since `1.15.2+etm14`**, and it was added for a consumer that **sums the
planes of a site** before storing — which is the right move, since the curve is compared with a
measured production that is the total of the roof.

Before: each plane carried its own last-refresh instant and drew its own jitter, so two planes
diverged within a few minutes. At each run publication, one would take the new run up to **one
hour** before the others — a careful consumer would have blocked its sum during that time, a
naive consumer would have written a chimera: part of the roof up to date, the other stale,
published as the whole.

What the consumer can therefore assume, and nothing more: **at a given instant, the curves of the
planes of the same site come from the same cycle and carry the same `issuedAt`.** Nothing is
guaranteed between two distinct sites.

### 5.8 What the curve does not say

It is a forecast converted by rule of three, with the factors of §0.1 ignored and all optimistic
— all five if `corrections` is empty, four if it contains `temperature`. **Overlaying it on the
measured production is the goal; deriving a coefficient from it remains forbidden** (§0.2). The
boundary does not move because both curves are displayed side by side — on the contrary, that is
where it will be most tempting to cross.

### 5.9 Version 2 (since `1.15.2+etm15`)

A change of shape, so `v` goes to **2** (rule of §5.1). All v1 fields are kept with the same
meaning; three change or are added:

| field | v1 | v2 |
|---|---|---|
| `corrections` | string `"none"` | **list**: `["temperature"]` or `[]`; never the string |
| `gti` | — | `gti[48]`, raw GTI in W/m², the API sample read by cell `i` |
| `tempC` | — | `tempC[48]`, `temperature_2m` in °C at instant `t0 + (i+1)·step`; `null` where the API gives none, **never 0** |

- **`p` is temperature-corrected** when `corrections` is `["temperature"]` (§2.6), and is exactly
  the v1 value when `corrections` is `[]`.
- `gti` and `tempC` serve for **recording** on the `ems` side: enough to redo or check the
  conversion later without depending on what it produced. They are not a second forecast.
- `gti` is always complete: without a complete GTI, no curve is published (as in v1). `tempC` may
  carry `null`s; it is then accompanied by `corrections: []`.
- No correction other than `temperature` is announced by a plane. In particular never `ac-limit`
  (site parameter, §2.6).

**Delivery order, mandatory**: the reader first. `ems` reads v1 and v2 since `1.15.2+etm66`; it
refuses a v2 whose `corrections` is absent or equal to `"none"`, any unknown correction, and
`ac-limit` coming from a plane. An earlier `ems` refuses any v2 (`VERSION_2`): this producer is
not deployed before it.

**`etm16` does not change v2**: shape, fields and computation identical, verified byte by byte
against the output of the `etm15` code on four real responses (test
`transportIsBitIdenticalToEtm15`). Only the **response** changes, through the elevation sent
(§3.4): measured effect nil on `p`, +0.1 K on one cell of `tempC`.

### 5.10 Versions of the structured payloads

| state | Thing | version | since | reader |
|---|---|---|---|---|
| `forecastCurveTransport` | plane | `v: 2` | `etm15` | `ems` (≥ `etm66`) |
| `observedGtiSeries` | plane | `v: 1` | `etm16` | `ems` (upcoming); offline analysis |
| `forecastHourlyWeather` | site | `v: 1` | `etm16` | app |

Common rule (§5.1): a consumer reads `v` and refuses an unknown version. All share `t0` (absolute
instant, Unix s), `step` (3600), cell `i` = `[t0 + i·step, t0 + (i+1)·step)`, and absence =
`null`. What differs is the sampling instant, written in `convention`: mean of the hour ending at
`t0 + (i+1)·step` for irradiance (transport, satellite), instantaneous value at `t0 + i·step` for
the hourly weather.

---

## 6. License and commercial use (to be decided outside the code)

The free API is declared **"for non-commercial use"**. Limits established **in the server code**
(`Sources/App/Helper/Vapor/RateLimiter.swift`, AGPLv3), not inferred from the documentation —
*"Count how many calls have been made by a given IP address"*: 600/min, 5,000/h, 10,000/day,
`dailyPerIPv4` keyed on `sin_addr.s_addr`, the address alone. Data under **CC BY 4.0**:
attribution mandatory. Self-hostable server with no code link to the plugin.

> **Caveat — "per IP" is not "per site".** Ten installations on ten distinct operator lines do
> have ten quotas. But behind **CGNAT** — common on 4G/5G and with some fibre ISPs — hundreds of
> subscribers share one public IPv4 and therefore one quota, with strangers, without anything
> signalling it before the first 429. At the hourly cadence a box uses a few dozen calls per day
> and the margin absorbs it, but **this is not guaranteed by construction**: it is a caveat, not
> a footnote. Degradation must stay clean — a 429 is handled as an absence of data, never as a
> value to reuse.

Three possible routes — free quota per installation, commercial key on
`customer-api.open-meteo.com`, self-hosting. The `apiKey` and `baseUrl` parameters (§4) exist so
that the choice remains a configuration and not a migration. The decision is taken before
commercialisation, not before the code.

---

## 7. Out of scope (explicitly)

- Learned local correction (K_climat, PR_site, K_dyn, shading mask) → ETM optimizer.
- Perez transposition → proprietary, never here. *(The NOCT thermal derating used to be on this
  line. It leaves it through the decision of 2026-10-12, `DESIGN_PREVISION_PV.md` §28.1: NOCT
  model with declared parameters, on the transport curve, §2.6. A **learned** thermal correction
  remains out of scope, previous line.)*
- AC cap / inverter clipping → `ems`, site parameter.
- Consumption forecast → optimizer.
- Long-term history → InfluxDB on the engine side.
- Any time series **learned or derived from site data** in a nymea state (CLAUDE.md). A series
  that comes out of a deterministic model or an observation relay is allowed and versioned
  (§5.10); the pre-integrated windows of §1.2 remain the default form for anyone who only needs a
  number.

---

## 8. Change log

| v1 | v2 | reason |
|---|---|---|
| "Live" section, states `ghi` / `gti` / `temperature` | "Current instant" section, everything prefixed `forecast` | Open-Meteo has no sensor on the site: these states were not measurements |
| `gti` **and** `expectedEnergyKwh` published | `forecastPvPower` alone + `forecastGtiSource` as a checkpoint | same information up to a factor, two apparent forecasts for a single one |
| `azimuth` "Open-Meteo convention" | "geographic convention" | wrong: Open-Meteo's south is 0 |
| `tendency` from Open-Meteo clear-sky | from `terrestrial_radiation` | `shortwave_radiation_clear_sky`: 0 non-zero values on the MF models |
| `performanceRatio` 0.80 by default | 1.00 | a coefficient hidden in a figure presented as a relay |
| live cadence 15 min | 1 h | runs come out every 3–6 h, published 4–5 h after init |
| no model fallback | `best_match` fallback, published in `forecastModel` | a silent fallback makes the announced accuracy lie |
| — | §0 regime, §1.2 crossing midnight, §2.2 measured indexing, §2.3 `is_day`, §6 license | additions in v2 of the contract |
| §2.2 "the shift would produce a gap of several percent" | two separate checks, only one of which discriminates | false claim: the day boundary is at midnight, zero irradiance, the shift changes no total there |
| energy rounding 0.5 kWh | 0.1 kWh | 0.5 kWh erased the useful distinction over a 3 h evening window |
| spread on the site | on the plane, computed on the GTI | CV(GTI) exceeds CV(GHI) by 2 to 4.6 pt, systematically, in the optimistic direction |
| — | §1.6: two ThingClasses, interface exception, `observed*` regime | the implementation has a site/plane model and a satellite source that the contract ignored |
| §1.1/§1.3/§4: `forecastCloudCover`, `forecastTemperature`, `lastUpdate`, `valid`, `solarWindowEnd`, `peakHour`, `todayTendency`, `peakPowerKwc`, `timezone`; power in kW without PR; §3.4 single request in ISO 8601; §3.1 "retry ×2"; §2.5 spread at `null` on failure; §5.5 "do not record" | names, types and defaults from the code; W with PR; three products, Unix seconds, elevation sent; retry every minute; last value kept; age as `NULL` | realignment on the `1.15.2+etm14` code before v2 of the transport (2026-10-13) |
| transport v1, `corrections:"none"`, temperature "proprietary", NOCT out of scope | transport v2 (§5.9): `p` temperature-corrected (§2.6), `gti[]`, `tempC[]`, `corrections` as a list; `tempCoeffPctPerK`, `noctC` on the plane | decision of 2026-10-12 (design §28.1 and §28.3); `etm15` |
| scalars by rule of three; elevation not sent with the plane request; retry every minute; spread kept without age; satellite GTI reduced to the last hour; no hourly weather | scalars corrected with the curve (§2.1); elevation on the plane request (§3.4); retry 5 min × 2^(n−1) (§3.1); `forecastWeatherSpreadTime`/`Valid`, NaN beyond 6 h (§1.5); `observedGtiSeries` (§1.7); `forecastHourlyWeather` (§1.8) | forecast follow-up and request from the app; `etm16` (2026-10-15) |
| `confidence` enum `low`/`medium`/`high` (considered) | `forecastWeatherSpreadPercent` in % + `forecastSpreadModel` | the Ensemble API is free and does expose the GTI, but a confidence enum reads as "reliability of the figure" whereas the spread ignores the five systematic biases of §0.1 |
