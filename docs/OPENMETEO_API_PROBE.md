# Open-Meteo API probes — MeteoFrance models

Probes run on **2026-09-08** against `https://api.open-meteo.com/v1/forecast`,
test point `48.85, 2.35` (Paris). All figures below come from actual responses
of the endpoint, not from the documentation.

Purpose: establish what the API actually provides, before Contract A was written.

---

## 1. Irradiance on a tilted plane (`global_tilted_irradiance`)

Available, computed **on the Open-Meteo server side** from GHI/DNI/DHI, via the
`tilt` and `azimuth` request parameters. No transposition is done in the
plugin: compliance with the license boundary (see CLAUDE.md).

Typical request:

    hourly=global_tilted_irradiance&tilt=30&azimuth=0&models=meteofrance_seamless

Availability per model (horizon = number of non-null hours from 00:00 local time):

| model | GTI | GTI horizon |
|---|---|---|
| `meteofrance_arome_france` | yes | 60 h |
| `meteofrance_arome_france_hd` | **no — 100 % null** | — |
| `meteofrance_arpege_europe` | yes | 111 h |
| `meteofrance_seamless` | yes | 111 h (≈ D+4.5) |
| `best_match` | yes | 384 h (non-MF blend) |

`terrestrial_radiation` (extraterrestrial radiation, astronomical) is the only
variable that is non-null over the 384 h of all models: it does not depend on any model.

**The national high-resolution model `arome_france_hd` exposes no radiation at all**
(neither `shortwave_radiation`, nor `direct_normal_irradiance`, nor `diffuse_radiation`,
nor GTI). It cannot be used as a solar source.

No direct PV production variable exists: `pv_power`, `photovoltaic_power`,
`solar_power`, `power_production` all return "unknown variable". The API provides
irradiance; conversion to power remains the consumer's job.

### 1.1 Preceding-hour average (integration pitfall)

`global_tilted_irradiance` is the **average over the hour preceding the timestamp**.
`global_tilted_irradiance_instant` is the instantaneous value at the timestamp.

Check (`meteofrance_seamless`, tilt 30 / azimuth 0, 2026-09-08):

| hour | average | instantaneous |
|---|---|---|
| 07:00 | 0.0 | 0.0 |
| 08:00 | 14.4 | 45.9 |
| 09:00 | 126.6 | 181.5 |

If the hourly value covered the *following* hour, the average at 08:00 would be ~110
(between 45.9 and 181.5), not 14.4.

**Consequence**: an energy integral over the next N hours must sum the steps
`t+1 … t+N`, not `t … t+N-1`. Otherwise there is a systematic one-hour shift.

### 1.2 Azimuth convention (confirmed empirically)

Vertical plane (`tilt=90`), GTI profile depending on `azimuth`:

| `azimuth` | peak | behaviour |
|---|---|---|
| `-90` | 10:00 | morning → **east** |
| `0` | afternoon | → **south** |
| `+90` | lingers until 20:00 | → **west** |

So the CLAUDE.md rule is correct: `azimuth_openmeteo = azimuth_geo - 180`
(geographic 180 = south → Open-Meteo 0; geo 90 = east → OM -90; geo 270 = west → OM +90).

Strict bounds, explicit HTTP rejection when out of range (no silent clamping):

    tilt=95   -> {"error":true,"reason":"Parameter `&tilt=` must be within 0° and 90°"}
    azimuth=200 -> {"error":true,"reason":"Parameter `&azimuth=` must be within -180° and 180°"}

### 1.3 Clear-sky: unavailable on MeteoFrance models

- `shortwave_radiation_clear_sky`: accepted by the API but **0 non-zero values**
  on `meteofrance_seamless` (returned unit: `"undefined"`).
- `global_tilted_irradiance_clear_sky`: does not exist (parsing error).

Any clearness index must therefore use `terrestrial_radiation` as the denominator,
the only reference available over the full horizon.

---

## 2. Horizon and granularity

- **Hourly**: 111 h on `meteofrance_seamless`, without an API key. D+1 amply covered.
- **15 minutes**: `minutely_15=global_tilted_irradiance` works, 443 non-null steps,
  the last at `2026-09-12T14:30` — same horizon as hourly.
- `forecast_days` beyond the model horizon does not produce an error: the excess
  steps are `null`. One must therefore test for null, not the array length.

---

## 3. Run cadence (source: `/data/<model>/static/meta.json`)

| model | run interval | resolution | run → publication latency |
|---|---|---|---|
| `meteofrance_arome_france_hd` | 3 h | 1 h | **5 h 08** |
| `meteofrance_arome_france_hd_15min` | 1 h | 15 min | — |
| `meteofrance_arpege_europe` | 6 h | 1 h | **4 h 21** |
| `meteofrance_arpege_europe_probabilities` | 12 h | 3 h | — |

Example reading: run initialized at 06:00 UTC, published at 11:08 UTC (AROME HD).

**Consequence**: querying more often than hourly only rereads the same data.
Useful cadence = 1 h, with jitter. The free quota (10,000 calls/day) is never
the limiting factor at this cadence.

The API returns no quota header (`X-RateLimit-*` absent): consumption cannot be
monitored other than by counting it on the plugin side.

---

## 4. Terms of use (pricing page, to be decided before commercialization)

- The free API is declared **"for non-commercial use"**, limited to
  600 calls/min, 5,000/h, 10,000/day, 300,000/month.
- Data under **CC BY 4.0**: attribution mandatory, including for commercial use.
- The Open-Meteo server itself is open source under **AGPLv3** (self-hosting
  possible, with no code link to the plugin).
- Paid option: endpoint `customer-api.open-meteo.com` with `&apikey=…`, same
  parameters and same response format.

**Design implication**: provide from V1 an optional `apiKey` parameter and
a configurable `baseUrl`. The three paths (free quota per installation, commercial
key, self-hosting) then become a configuration change rather than
a migration.

---

## 5. Unverified points

- Accessibility of the Ensemble API (model spread, for a possible `confidence`)
  on the free tier: the pricing page is ambiguous on this point.

---

## 6. Ensemble API — probes of 2026-09-08

Endpoint `https://ensemble-api.open-meteo.com/v1/ensemble`.

**Accessible without an API key** (HTTP 200) and **exposes `global_tilted_irradiance` per
member**, with the `tilt` / `azimuth` parameters — the spread therefore applies to the
published quantity, not to horizontal radiation.

### 6.1 Models exposing GTI

| ensemble | members with GTI |
|---|---|
| `ecmwf_ifs025` | 51 |
| `icon_seamless`, `icon_eu` | 40 |
| `gfs_seamless`, `gfs025` | 31 |
| `gem_global` | 21 |
| `icon_d2` | 20 |
| `ukmo_global_ensemble_20km` | 18 |
| `ecmwf_ifs04` | 0 |
| `bom_access_global_ensemble` | 0 |

**No MeteoFrance ensemble.** `meteofrance_arpege_ensemble`,
`meteofrance_arpege_europe_probabilities` and `meteofrance_arome_france_ensemble` are
all rejected by the ensemble endpoint (`invalid String value ... MultiDomains`).

Cadence: `ecmwf_ifs025_ensemble` = run every 6 h, 3 h resolution.
`icon_seamless` provides hourly GTI over 4 days (96 steps).

### 6.2 Measured spread — daily POA energy, ICON ensemble (39 members + control)

| day | median | min | max | std. dev. | CV |
|---|---|---|---|---|---|
| 2026-09-08 | 3.96 | 2.71 | 4.80 | 0.46 | 12 % |
| 2026-09-09 | 5.01 | 3.67 | 6.21 | 0.60 | 12 % |
| 2026-09-10 | 6.21 | 4.43 | 7.03 | 0.62 | 10 % |
| 2026-09-11 | 3.10 | 0.90 | 6.41 | 1.24 | **38 %** |

(kWh/m², plane 30° / south). The quantity discriminates: D+3 is genuinely undecidable.

### 6.3 MF ↔ ICON cross-consistency check

Position of the published MeteoFrance forecast within the ICON ensemble envelope:

| day | MF published | ICON envelope | MF percentile | inside the envelope |
|---|---|---|---|---|
| 2026-09-08 | 3.32 | 2.71 – 4.80 | 15 % | yes |
| 2026-09-09 | 4.92 | 3.67 – 6.21 | 41 % | yes |
| 2026-09-10 | 6.30 | 4.43 – 7.03 | 59 % | yes |
| 2026-09-11 | 3.24 | 0.90 – 6.41 | 54 % | yes |

**Scope of this check: weak.** One site, four days. It does not validate the
qualification of an MF forecast by an ICON ensemble — it only notes that the forecast
is not contradicted on this sample. The rejection hypothesis ("MF falls outside the
envelope") was tested and is not confirmed. To be re-examined on a larger
sample if the question becomes decision-relevant again.

### 6.4 What these probes do not tell

None of these figures says anything about the systematic conversion biases
(shading, soiling, module temperature, clipping). Ensemble spread is
a random meteorological error; the biases of §0.1 of Contract A are systematic
and of constant sign. See CONTRACT_A §2.5.

---

## 7. Satellite endpoint and actual scope of the integration check (2026-09-08)

### 7.1 `current=` is silently ignored by the satellite endpoint

    GET https://satellite-api.open-meteo.com/v1/archive
        ?models=satellite_radiation_seamless
        &current=shortwave_radiation,shortwave_radiation_clear_sky

Response: **HTTP 200, no `error` field, and no `current` block** — only the
metadata (`latitude`, `elevation`, `timezone`…). The states fed from this block
were therefore never filled, without any message.

With `hourly=` + `past_days=1`, the same endpoint answers normally. Measured latency of the
last non-null sample: **59 min**.

It additionally accepts `global_tilted_irradiance` with `tilt` / `azimuth` — an
**observed** GTI, distinct from the forecast GTI.

### 7.2 Clear-sky exists on the satellite side

Unlike the forecast API on MeteoFrance models (§1.3, 0 non-zero values), the
satellite product provides a real clear-sky: 495.8 W/m² observed against 694.5
clear-sky, i.e. kt = 0.71.

### 7.3 Ensemble spread: GTI and GHI are not interchangeable

CV of daily energy, ICON ensemble, plane 30° / south:

| day | CV(GHI) | CV(GTI) | difference |
|---|---|---|---|
| 2026-09-08 | 10.0 % | 11.9 % | 2.0 pt |
| 2026-09-09 | 10.1 % | 12.0 % | 1.9 pt |
| 2026-09-10 | 7.8 % | 10.3 % | 2.5 pt |
| 2026-09-11 | 33.8 % | 38.4 % | 4.6 pt |

CV(GTI) systematically exceeds CV(GHI), and the difference grows with the spread. Computing
the spread on GHI would underestimate it — in the optimistic direction.

### 7.4 Correction: what the daily identity of §1.1 proves, and what it does not

A first analysis concluded that the 0.1 % agreement between the hourly sum and
`daily.shortwave_radiation_sum` demonstrated the backward-looking convention, on the grounds that a
forward-looking convention would produce "several percent of difference at the day boundary".

**This is wrong.** The day boundary falls at midnight, where irradiance is zero: shifting
the summation by one hour at that point changes no total. Verified by deliberately breaking
the convention — the identity still passes at 0.05 %.

What the identity actually establishes: the hourly values are true **hourly
averages** (and not instantaneous samples), and our day boundaries are correct.

The discriminating proof lies elsewhere — `shortwave_radiation` against
`shortwave_radiation_instant`, two independently published series:

| hypothesis | mean absolute error (37 samples) |
|---|---|
| backward-looking `(HH−1, HH]` | **26.0 W/m²** |
| forward-looking `(HH, HH+1]` | 83.2 W/m² |

Ratio 0.31. Both checks appear separately in
`openmeteo/tests/check_integration_convention.py`, each validated by a negative control.
