# MAPPING — Contract A → nymea metadata (openmeteo)

Translation of [Contract A](CONTRACT_A_openmeteo.md) into
`openmeteo/integrationpluginopenmeteo.json`, which is authoritative. `plugininfo.h` is
**generated** from it — never edit it by hand (CLAUDE.md).

Form constraints applied throughout:

- **Scalar states only.** No time series in a state. The API series are integrated at
  write time (§1.2/1.3 of the contract) and only the scalar is published.
- **Irradiances: no unit.** nymea has no `WattPerSquareMeter` → `unit` is omitted
  and "(W/m²)" appears in the `displayName`.
- **Azimuth stored as geographic** (180 = south), converted when sent:
  `azimuth_openmeteo = azimuth_geo − 180`.
- Existing UUIDs are **preserved** when renaming: a stable `id` avoids breaking
  Things that are already configured.

---

## 0. The two-level model, absent from Contract A

The contract described a single ThingClass. The plugin has two, and that is better:

- **`openMeteoSite`** (parent) — the weather of the location. Coordinates, one shared request.
- **`openMeteoPvPlane`** (child) — a photovoltaic plane: `tilt`, `azimuth`, `kwp`.
  Several orientations = several children, each with its own GTI and integrals.

The contract is implicitly amended on this point: the energy states of §1.2/1.3
live on the **plane**, not on the site, since they depend on the orientation and the
peak power. The site carries the weather, the spread and the satellite observation.

### 0.1 The parent-child link, and what it requires

A plane reads its coordinates from its site: it does not carry them itself. Two
consequences, both observed on a test installation, not inferred.

**The plane is created by discovery, not by manual entry.** `Integrations.AddThing` carries
**no `parentId`** in JSON-RPC protocol 9.0 (checked by introspection). A plane
declared `createMethods: ["user"]` could therefore never know its site:
`resolveCoordinates()` would have failed on every cycle and no state would have been published —
without any error, neither at compile time nor at load time. The plane therefore uses
`createMethods: ["discovery"]`, and `discoverThings()` returns one descriptor per configured
site, carrying the `parentId`. nymea applies it at creation
(`thingmanagerimplementation.cpp:375`). No `thingId` on the descriptor: a site can
carry several planes, one per roof orientation.

**Moving a site must carry its planes along.** nymea calls `setupThing()` again on a
reconfiguration (`:473`), but only for the reconfigured Thing. Without propagation, a
plane kept computing PV for the previous location until its own hourly
tick — up to one hour of plausible kWh for the wrong place.

Propagation is limited to `info->isReconfigure()`. **`isInitialSetup()` does not discriminate**:
the reconfiguration path builds its `ThingSetupInfo` with
`initialSetup=true` *and* `reconfigure=true` (`:473`), whereas normal pairing passes
`reconfigure=false` (`:2239`). Without this guard, every nymead restart doubled
the three requests of each plane, which already runs its own `setupThing()`.

Check: on restart as well as after a reconfiguration of the site alone, the log
must show **exactly five requests, one of each** — two for the site (forecast,
satellite), three for the plane (forecast GTI, satellite GTI, spread).

---

## 1. Exception to the `forecast` prefix — nymea interfaces impose their names

§0 of Contract A requires **every** state to be prefixed `forecast`. This rule is
**not applicable as such**: `openMeteoSite` declares the `weather` and
`daylightsensor` interfaces, which impose exact state names.

Checked in `libnymea/interfaces/weather.json` and `daylightsensor.json`:

| interface | imposed states |
|---|---|
| `weather` | `weatherDescription`, `weatherCondition`, `temperature`, `humidity`, `pressure`, `windSpeed`, `windDirection` |
| `daylightsensor` | `daylight`, `sunriseTime`, `sunsetTime` |

Renaming these ten states to `forecastTemperature`, `forecastHumidity`… would break
interface conformance: the nymea app and any generic consumer would stop
recognizing the Thing as a weather source.

**Decision taken.** The prefix applies to everything the contract controls, and
only to that:

| category | naming rule | reason |
|---|---|---|
| 10 `weather` / `daylightsensor` interface states | imposed name, **not prefixed** | interface conformance; these are convenience weather quantities, not the basis of an energy decision |
| forecast irradiances and energies | **`forecast`** prefix | these are the states the engine reads; §0 applies in full |
| quantities derived from the satellite | **`observed`** prefix | different regime, explicit distinction |
| `terrestrialRadiation` | neither | astronomical quantity, neither forecast nor observed |

The residual risk is bounded and accepted: an unprefixed `temperature` can be read as
a measurement. It is an **air** temperature, never used in an energy
conversion, and the contract nowhere allows it as a decision input. The prefix
protects where confusion is costly — the kWh.

---

## 2. Two regimes, and the satellite that was not in the contract

The existing skeleton already queried `satellite-api.open-meteo.com`
(`satellite_radiation_seamless`), an **observation** source that Contract A ignored.
The plugin therefore carries two regimes, and the prefix separates them:

| prefix | regime | source | latency |
|---|---|---|---|
| `forecast*` | forecast | `api.open-meteo.com`, `meteofrance_seamless` | published 4–5 h after the run |
| `observed*` | satellite observation | `satellite-api.open-meteo.com` | **≈ 59 min** (measured) |

`observedTime` and `observedGtiTime` publish the timestamp of the satellite sample,
not that of the request: the one-hour latency must be visible, otherwise a one-hour-old
observation reads as a snapshot.

### 2.1 Bug fixed along the way: `current=` ignored by the satellite endpoint

The code carried `TODO(verify): confirm the satellite endpoint supports "current="`.
Measured answer: **no**, and the failure is silent — the API returns HTTP 200, no
`error` field, and simply **no `current` block**. The states `ghiObserved`,
`clearSkyGhi` and `clearSkyIndex` were therefore never filled.

Fix: `hourly=` + `past_days=1`, then **last non-null sample**. Verified
working, and the satellite endpoint additionally accepts `global_tilted_irradiance` with
`tilt`/`azimuth` — hence a per-plane `observedGti`, which did not exist.

### 2.2 Clear-sky exists on the satellite side

§2.4 of the contract notes that `shortwave_radiation_clear_sky` returns 0 non-zero values
on the MeteoFrance models of the forecast API. **This is true of the forecast API
only.** The satellite product does provide it (probe: 495.8 observed against
694.5 clear-sky → kt = 0.71).

Consequence for the contract: two distinct indices, not to be confused.

| state | definition | regime |
|---|---|---|
| `observedClearSkyIndex` | observed GHI / observed clear-sky | observation, present instant |
| `forecastTendencyToday` | Σ forecast GHI / Σ `terrestrial_radiation` | forecast, day |

The second keeps `terrestrial_radiation` as the denominator: satellite clear-sky exists
only for the past, it cannot qualify a forecast.

---

## 3. Mapping table — `openMeteoSite`

| Contract A | nymea state | type | unit | API source |
|---|---|---|---|---|
| — | `weatherDescription` | QString | — | `weather_code` (imposed by the interface) |
| — | `weatherCondition` | QString | — | `weather_code` |
| `forecastTemperature` | `temperature` | double | DegreeCelsius | `current.temperature_2m` |
| — | `temperatureMin` / `temperatureMax` | double | DegreeCelsius | `daily.temperature_2m_{min,max}[0]` |
| — | `humidity` | int | Percentage | `current.relative_humidity_2m` |
| — | `pressure` | double | HectoPascal | `current.surface_pressure` |
| — | `windSpeed` / `windDirection` | double / int | MeterPerSecond / Degree | `current.wind_speed_10m`, `wind_direction_10m` |
| `forecastCloudCover` | `cloudiness` | int | Percentage | `current.cloud_cover` |
| — | `snowfall` | double | CentiMeter | `current.snowfall` |
| — | `forecastGhi` | double | *(omitted)* "(W/m²)" | `current.shortwave_radiation` |
| — | `forecastDni` | double | *(omitted)* | `current.direct_normal_irradiance` |
| — | `forecastDhi` | double | *(omitted)* | `current.diffuse_radiation` |
| — | `terrestrialRadiation` | double | *(omitted)* | `current.terrestrial_radiation` |
| — | `forecastGhiSumToday` | double | *(omitted)* "(kWh/m²)" | `daily.shortwave_radiation_sum[0]` ÷ 3.6 (MJ → kWh) |
| §1.5 | `forecastWeatherSpreadTodayPercent` | double | Percentage | ensemble, §2.5 of the contract |
| §1.5 | `forecastWeatherSpreadTomorrowPercent` | double | Percentage | ensemble |
| §1.5 | `forecastSpreadModel` | QString | — | effective ensemble model |
| §1.1 | `forecastModel` | QString | — | model that answered (§3.2) |
| §1.1 `valid` | `forecastValid` | bool | — | `lastUpdate` > 3 h → false |
| §1.3 `todayTendency` | `forecastTendencyToday` | QString | — | Σ GHI / Σ TOA |
| — | `observedGhi` | double | *(omitted)* | satellite `hourly.shortwave_radiation`, last non-null |
| — | `observedClearSkyGhi` | double | *(omitted)* | satellite `hourly.shortwave_radiation_clear_sky` |
| — | `observedClearSkyIndex` | double | — | ratio of the two, night guard |
| — | `observedTime` | int | UnixTime | timestamp of the satellite sample |
| — | `sunriseTime` / `sunsetTime` | int | UnixTime | `daily.sunrise/sunset[0]` (imposed) |
| §1.3 `solarWindowEnd` | `sunsetTime` | int | UnixTime | same state, no duplicate |
| — | `daylight` | bool | — | `current.is_day` (imposed) |
| — | `updateTime` | int | UnixTime | `current.time` |

## 4. Mapping table — `openMeteoPvPlane`

| Contract A | nymea state | type | unit | derivation |
|---|---|---|---|---|
| §1.1 `forecastPvPower` | `forecastPvPower` | double | Watt | `GTI_instant × kwp × PR` |
| §1.1 `forecastGtiSource` | `forecastGtiSource` | double | *(omitted)* "(W/m²)" | `current.global_tilted_irradiance` |
| §1.2 | `forecastEnergyNext3h` | double | KiloWattHour | Σ `hourly` over `t+1…t+3` |
| §1.2 | `forecastEnergyNext6h` | double | KiloWattHour | same, `t+1…t+6` |
| §1.2 | `forecastEnergyNext12h` | double | KiloWattHour | same, `t+1…t+12` |
| §1.3 | `forecastEnergyRestOfDay` | double | KiloWattHour | Σ from `t+1` to `sunsetTime` |
| §1.3 | `forecastEnergyTomorrow` | double | KiloWattHour | Σ over day D+1 |
| §1.3 `peakHour` | `forecastPeakTime` | int | UnixTime | argmax of the day's hourly GTI |
| — | `observedGti` | double | *(omitted)* | satellite, same `tilt`/`azimuth` |
| — | `observedGtiTime` | int | UnixTime | timestamp of the satellite sample |
| — | `updateTime` | int | UnixTime | `current.time` |

Plane parameters: `tilt` (0–90), `azimuth` (geographic, 0–360), `kwp`,
and **`performanceRatio` added, default `1.00`** (§2.1 of the contract: no hidden
coefficient; below 1, it is a derating declared by the installer).

### 4.1 No `observedPvPower`, deliberately

The satellite provides `observedGti`, and the same rule of three would produce an
`observedPvPower`. It is **not** published.

Reason: this state would sit next to a production actually measured by the inverter,
and would invite exactly the forecast/measured comparison that §0.2 of the contract forbids —
that is learning, it belongs to the proprietary scope. The asymmetry with the
forecast branch (where the power is published, not the irradiance) is accepted: on the
forecast side there is no competing measurement, on the observation side there is.

---

## 5. Plugin parameters

| parameter | default | role |
|---|---|---|
| `baseUrl` | `https://api.open-meteo.com` | forecast; self-hosting (§6 of the contract) |
| `satelliteBaseUrl` | `https://satellite-api.open-meteo.com` | observation |
| `ensembleBaseUrl` | `https://ensemble-api.open-meteo.com` | spread |
| `weatherModel` | `meteofrance_seamless` | nominal model |
| `ensembleModel` | `icon_seamless` | 40 members, hourly GTI, 4 days |
| `apiKey` | *(empty)* | if set → commercial customer endpoint |

## 6. Update intervals (§3.1 of the contract)

A single `PluginTimer` at 900 s; each source is then triggered at its own interval,
because none of them refreshes every 15 min:

| source | interval | measured reason |
|---|---|---|
| forecast | 1 h | runs every 3–6 h, published 4–5 h after init |
| satellite | 1 h | latency ≈ 59 min |
| ensemble | 3 h | runs every 6 h |

Jitter ±60 s. The `refreshWeather` action forces all three, ignoring the intervals.

---

## 7. Non-regression test — imposed form

§2.2 of the contract imposes the form: **plugin sum against the API's daily
aggregate**, never plugin against plugin. It is the only check whose two terms do not
share their source.

`openmeteo/tests/check_integration_convention.py` compares the plugin's hourly
summation, over the same day boundaries as those coded in the `.cpp`
(`J 01:00 → J+1 00:00`, backward-looking average), with the `daily.shortwave_radiation_sum`
computed by Open-Meteo. Failure threshold: **1 %**. The value read on 2026-09-08 was 0.1 %.

This test would fail if someone "corrected" the indexing to `t…t+N-1`, which is
precisely its purpose.
