# OpenMeteo

This plugin allows to get current weather and solar irradiance data from [Open-Meteo](https://open-meteo.com), including plane-of-array irradiance (GTI) for photovoltaic surfaces and satellite-observed clear-sky data.

## Usage

Each upstream product is polled at its own rate, because none of them refreshes every 15 minutes: forecast every hour (model runs are 3-6 h apart and published 4-5 h after initialisation), satellite observation every hour (measured latency ~59 min), ensemble spread every 3 hours, each with +/-60 s of jitter. A manual refresh forces all three.

A source that fails is retried after 5 min, then 10, 20, 40... (doubling at each consecutive failure, capped at the source's own interval, +/-60 s of jitter), and back to its normal rate at the first success. A reply that carries no data counts as a failure. The forecast of a site and of all its planes is one source, retried together so that the planes keep sharing one instant.

> **Everything this plugin publishes is a forecast or a satellite observation, never a measurement of your array.** States prefixed `forecast` are predictions; states prefixed `observed` come from satellite observation, about an hour old. The PV power figure is `GTI x kWp x PR` and ignores shading, soiling, module temperature and inverter clipping - all of which push it optimistically high. Do not treat 4 kW forecast as 4 kW produced. Module temperature is corrected, with the coefficient and NOCT declared on the plane, in the hourly curve handed to the energy manager (`forecastCurveTransport`) and - since 1.15.2+etm16 - in the PV power and energy states as well: both are corrected together, or neither when the temperature series is incomplete. The other three are still ignored.

A **site** is added by entering its latitude and longitude (no discovery). Both are required: a site whose coordinates are left at the default (999), or outside -90..90 / -180..180, is not set up, sends no request, and setup reports "Enter the site coordinates". Its PV planes do not set up either. 0 / 0 is a real point and is accepted. It provides the shared weather and solar data for the location, including a satellite-observed GHI and clear-sky index.

The site's elevation, when entered, is sent with the forecast requests of the site and of each of its planes.

One or more **PV planes** can be added as children of a site. Each plane has its own tilt, azimuth and peak power, and reports the global tilted irradiance (GTI) for that orientation. Multiple orientations are handled by adding multiple planes. The power temperature coefficient (default -0.37 %/K) and NOCT (default 45 °C) are module datasheet values; existing planes take the defaults.

> Note: the azimuth is entered in geographic convention (180 = South, 90 = East, 270 = West).

By default the plugin queries the public Open-Meteo API. For commercial deployments, the `Base URL` and `Base URL (satellite radiation)` plugin settings can be pointed to a self-hosted Open-Meteo instance (the public API is for non-commercial use only).

## Supported Things

* Open-Meteo site (interfaces: weather, daylightsensor)
    * Weather condition
    * Temperature (current, daily min and max)
    * Humidity
    * Pressure
    * Wind speed and direction
    * Cloudiness
    * Snowfall
    * Global, direct normal and diffuse irradiance (GHI / DNI / DHI)
    * Terrestrial (top-of-atmosphere) radiation
    * Daily shortwave radiation sum
    * Satellite-observed GHI, clear-sky GHI and clear-sky index
    * Sunrise and sunset time
    * Daylight
    * Hourly weather for today and tomorrow (`forecastHourlyWeather`): WMO weather code,
      day/night and temperature per hour, one versioned structure for the app. Each value
      is the one at the start of its hour; a missing hour is null, never 0 (code 0 is a
      clear sky)

* Open-Meteo PV plane
    * Forecast GTI for the configured tilt and azimuth, and the PV power derived from it
    * Forecast PV energy over rolling 3 h, 6 h and 12 h windows (these cross midnight by
      construction - the state that answers "what is left today" is the rest-of-day one)
    * Forecast PV energy for the rest of today and for tomorrow, and the peak time
    * Satellite-observed GTI for the same orientation: the last hour, and the hourly series
      of yesterday and today (`observedGtiSeries`)
    * Weather spread from an ensemble model, in percent. This qualifies the **weather**,
      not the accuracy of the PV figure: it measures meteorological uncertainty and is
      blind to shading, soiling and module temperature. On a clear stable day it reads
      low at the very moment the conversion bias is highest. It is dated
      (`forecastWeatherSpreadTime`), and when no spread has been computed for more than
      6 hours `forecastWeatherSpreadValid` turns false and the percentages become invalid
      (null over the API, never 0).
    * The hourly forecast curve for the energy manager (`forecastCurveTransport`)

The structured states (`forecastCurveTransport` v2, `observedGtiSeries` v1, `forecastHourlyWeather` v1) carry absolute instants: slot i covers `[t0 + i*3600, t0 + (i+1)*3600)`. Their exact form and time conventions are in `docs/CONTRACT_A_openmeteo.md` (sec. 1.7, 1.8, 5).

## Tests

Offline, against recorded replies (no network): `tests/pvcurve`, `tests/retryschedule`, `tests/hourlyseries`, `tests/ensemblespread`. In each: `qmake6 && make && ./tst_<name>`. They are not run by the package build.

## Requirements

* Internet connection, or a reachable self-hosted Open-Meteo instance
* The package 'nymea-plugin-openmeteo' must be installed
* No API key is required. For commercial use, a self-hosted instance is required (the public Open-Meteo API is non-commercial only).

## More

Open-Meteo https://open-meteo.com

Weather data by Météo-France, satellite radiation by EUMETSAT, provided through Open-Meteo (CC-BY 4.0).

For the overall architecture (role of this plugin, conventions, license boundary), see the `etm-powersync-docs` repository.
