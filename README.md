# etm-powersync-plugins

nymea integration plugins maintained by ETM-Schurig for the ETM PowerSync home energy
management system.

This repository is a fork of [nymea-plugins](https://github.com/nymea/nymea-plugins): its Git
history descends from upstream. It only keeps the plugins that ETM builds and packages; all the
other nymea plugins are not in this tree and come from the upstream nymea packages. Packages are
built against nymea 1.15.2 and versioned `1.15.2+etmN`.

## Versions

Packages are built from the release branches of the ETM Git server and versioned
`1.15.2+etmN`. `debian/changelog` lists what each version changes.

## Plugins and packages

| Directory | Debian package | What it does |
|---|---|---|
| `openmeteo` | `powersync-plugin-openmeteo` | ETM plugin. Fetches weather, solar irradiance and PV forecasts from [Open-Meteo](https://open-meteo.com) for a site and its PV planes (tilt, azimuth, peak power). |
| `trydan` | `powersync-plugin-v2c` | ETM plugin. Drives V2C Trydan EV chargers over their local HTTP API: pause and resume, charging current, single / three phase mode. |
| `genericenergy` | `powersync-plugin-generic-energy` | Fork of the nymea generic energy plugin (upstream 1.15.2): fixes the removal of impulse smart meters and integrates current power into the energy counters. |
| `edf` | `powersync-plugin-edf` | ETM plugin. Reads a Linky meter (TIC, historic mode) on a serial port and describes the electricity contract (Base, off-peak / peak hours, EJP): price of the current period, next switch, price slots. Computed on the box, no network connection. |

The fork `genericenergy` provides, replaces and conflicts with the matching `nymea-plugin-*` package, so going
back to upstream is a package swap. Each directory has a `README.md` with the details, including
what differs from upstream.

The APT channel of each device is listed in the
[compatibility matrix](https://docs.etm-powersync.fr/en/appareils/compatibilite/) of the
documentation.

## Build

Build dependencies (`debian/control`): `debhelper`, `pkg-config`, `libnymea-dev`,
`nymea-dev-tools`, `qt6-base-dev`, `qt6-base-dev-tools`. The plugins are built with qmake (Qt 6)
against the nymea `plugin.pri` installed by `libnymea-dev`; `plugininfo.h` is generated from the
`.json` metadata of each plugin.

Debian packages:

```sh
dpkg-buildpackage -b -uc -us
```

A subset of plugins can be built with `qmake WITH_PLUGINS="openmeteo"` or
`WITHOUT_PLUGINS="…"`.

## Tests

`openmeteo/tests/` and `trydan/tests/` hold offline Qt Test programs (recorded replies, a local
fake charger; no network, no device). They are not part of the package build. In each test
directory:

```sh
qmake6 && make && ./tst_<name>
```

`edf/tests/` is run with its own script:

```sh
./edf/tests/run-tests.sh
```

## Documentation

User and installer documentation: <https://docs.etm-powersync.fr>

## License

GPL-3.0-or-later (SPDX headers of the sources). The full text is in `COPYING` and
`LICENSE.GPL3`.
