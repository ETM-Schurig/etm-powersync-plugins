// SPDX-License-Identifier: GPL-3.0-or-later

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
* Copyright (C) 2026, ETM-Schurig SARL
*
* This file is part of etm-powersync-plugins.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <https://www.gnu.org/licenses/>.
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "integrationpluginopenmeteo.h"
#include "plugininfo.h"
#include "pvcurve.h"
#include "hourlyseries.h"
#include "ensemblespread.h"
#include "sitecoordinates.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QVariantMap>
#include <QUrl>
#include <QUrlQuery>
#include <QSharedPointer>

#include <integrations/thing.h>
#include <network/networkaccessmanager.h>

// NOTE: the *StateTypeId / *ParamTypeId / *ThingClassId / *ActionTypeId / dcOpenMeteo
// identifiers below are generated into plugininfo.h from integrationpluginopenmeteo.json.
// If a name does not match, align it with the generated header (do not edit plugininfo.h).
//
// Regime rule (CONTRACT_A sec. 0): everything this plugin publishes is either a forecast
// (forecast* states) or a satellite observation (observed* states). Neither is a
// measurement of the array. The ten states mandated by the nymea "weather" and
// "daylightsensor" interfaces keep their interface names and are the documented
// exception - see docs/MAPPING_openmeteo.md sec. 1.

IntegrationPluginOpenMeteo::IntegrationPluginOpenMeteo()
{
}

void IntegrationPluginOpenMeteo::init()
{
    // No API key required for the free tier. The apiKey plugin parameter targets the
    // commercial endpoint; baseUrl/satelliteBaseUrl/ensembleBaseUrl allow self-hosting.
    // See CONTRACT_A sec. 6 - the free API is declared non-commercial use only.
}

const char *IntegrationPluginOpenMeteo::FallbackModel = "best_match";

QString IntegrationPluginOpenMeteo::configuredModel() const
{
    return configValue(openMeteoPluginWeatherModelParamTypeId).toString();
}

bool IntegrationPluginOpenMeteo::radiationAllNull(const QVariantMap &current, const QVariantMap &hourly,
                                                  const QStringList &keys)
{
    foreach (const QString &key, keys) {
        if (current.contains(key) && !current.value(key).isNull())
            return false;
        foreach (const QVariant &sample, hourly.value(key).toList()) {
            if (!sample.isNull())
                return false;
        }
    }
    return true;
}

double IntegrationPluginOpenMeteo::azimuthToOpenMeteo(double azimuthGeographic)
{
    // Geographic 180 = South maps to Open-Meteo 0; 90 (East) to -90; 270 (West) to +90.
    // The API rejects anything outside [-180, 180] with an HTTP error rather than
    // clamping silently, so normalise before sending.
    double a = azimuthGeographic - 180.0;
    while (a > 180.0)
        a -= 360.0;
    while (a < -180.0)
        a += 360.0;
    return a;
}

void IntegrationPluginOpenMeteo::addCommonQuery(QUrlQuery &query, double lat, double lon) const
{
    // Coordinates are rounded to 2 decimals (~1 km): enough for weather, blurred enough
    // not to ship the exact address (CONTRACT_A sec. 3.3).
    query.addQueryItem("latitude", QString::number(lat, 'f', 2));
    query.addQueryItem("longitude", QString::number(lon, 'f', 2));
    query.addQueryItem("timezone", "Europe/Paris");
    query.addQueryItem("timeformat", "unixtime");
    const QString apiKey = configValue(openMeteoPluginApiKeyParamTypeId).toString();
    if (!apiKey.isEmpty())
        query.addQueryItem("apikey", apiKey);
}

void IntegrationPluginOpenMeteo::addElevation(QUrlQuery &query, Thing *site) const
{
    // Declared by the installer on the site; 0 = not declared, not sent (the API then uses
    // the altitude of its own terrain model). Sent only on the forecast requests, whose
    // temperature depends on it - the satellite and ensemble requests carry irradiance only.
    if (!site)
        return;
    const double elevation = site->paramValue(openMeteoSiteThingElevationParamTypeId).toDouble();
    if (elevation != 0.0)
        query.addQueryItem("elevation", QString::number(elevation));
}

QString IntegrationPluginOpenMeteo::scheduleKey(Thing *thing, const char *source)
{
    return thing->id().toString() + QLatin1Char('/') + QLatin1String(source);
}

void IntegrationPluginOpenMeteo::discoverThings(ThingDiscoveryInfo *info)
{
    if (info->thingClassId() != openMeteoPvPlaneThingClassId) {
        info->finish(Thing::ThingErrorThingClassNotFound);
        return;
    }

    ThingDescriptors descriptors;
    foreach (Thing *site, myThings()) {
        if (site->thingClassId() != openMeteoSiteThingClassId)
            continue;
        // No thingId on the descriptor: a site may carry several planes (one per roof
        // orientation), so each discovery must be addable rather than matched to an
        // existing thing.
        ThingDescriptor descriptor(openMeteoPvPlaneThingClassId,
                                   tr("PV plane"),
                                   tr("At site %1").arg(site->name()),
                                   site->id());
        descriptors.append(descriptor);
    }

    if (descriptors.isEmpty())
        qCWarning(dcOpenMeteo()) << "No Open-Meteo site configured: add a site before a PV plane.";

    info->addThingDescriptors(descriptors);
    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginOpenMeteo::setupThing(ThingSetupInfo *info)
{
    Thing *thing = info->thing();

    // A site without entered coordinates is not set up (CONTRACT_A sec. 4): the declared
    // default is a sentinel out of range, so a site added without touching the fields
    // lands here, and no request leaves for a placeholder point.
    double lat, lon;
    if (thing->thingClassId() == openMeteoSiteThingClassId) {
        if (!resolveCoordinates(thing, lat, lon)) {
            info->finish(Thing::ThingErrorInvalidParameter,
                         QT_TR_NOOP("Enter the site coordinates: latitude from -90 to 90, longitude from -180 to 180."));
            return;
        }
    } else if (thing->thingClassId() == openMeteoPvPlaneThingClassId) {
        if (!siteOf(thing)) {
            info->finish(Thing::ThingErrorHardwareNotAvailable, QT_TR_NOOP("Parent site not available."));
            return;
        }
        if (!resolveCoordinates(thing, lat, lon)) {
            info->finish(Thing::ThingErrorHardwareNotAvailable,
                         QT_TR_NOOP("Enter the coordinates of the parent site first."));
            return;
        }
    }

    // Read before finish(): the info object is not ours to keep afterwards.
    const bool reconfigure = info->isReconfigure();

    info->finish(Thing::ThingErrorNoError);

    refresh(thing, true);

    // A plane reads its coordinates from its parent site. When a site is reconfigured
    // (nymea calls setupThing again, thingmanagerimplementation.cpp:473), its planes must
    // re-request too, or they keep computing PV for the previous location until their own
    // hourly tick - up to an hour of plausible numbers for the wrong place.
    //
    // Only on reconfigure. On a plain setup - including every nymead restart - each plane
    // runs its own setupThing, and propagating here as well would double all of their
    // requests. Note isInitialSetup() cannot be used for this: the reconfigure path passes
    // initialSetup=true as well, so isReconfigure() is the only one that discriminates.
    if (reconfigure && thing->thingClassId() == openMeteoSiteThingClassId) {
        foreach (Thing *child, myThings()) {
            if (child->thingClassId() == openMeteoPvPlaneThingClassId && child->parentId() == thing->id())
                refresh(child, true);
        }
    }

    if (!m_pluginTimer) {
        // One tick a minute. Each source is gated on its own interval; the cost of the
        // extra ticks is one hash lookup and a comparison per thing per minute, against
        // a fleet that would otherwise hit the API on the same quarter-hour grid.
        m_pluginTimer = hardwareManager()->pluginTimerManager()->registerTimer(60);
        connect(m_pluginTimer, &PluginTimer::timeout, this, [this]() {
            foreach (Thing *thing, myThings()) {
                refresh(thing);
            }
        });
    }
}

void IntegrationPluginOpenMeteo::executeAction(ThingActionInfo *info)
{
    Thing *thing = info->thing();

    if (thing->thingClassId() == openMeteoSiteThingClassId
            && info->action().actionTypeId() == openMeteoSiteRefreshWeatherActionTypeId) {
        refresh(thing, true);
        foreach (Thing *child, myThings()) {
            if (child->thingClassId() == openMeteoPvPlaneThingClassId && child->parentId() == thing->id())
                refresh(child, true);
        }
    }

    info->finish(Thing::ThingErrorNoError);
}

void IntegrationPluginOpenMeteo::thingRemoved(Thing *thing)
{
    m_schedule.remove(scheduleKey(thing, "forecast"));
    m_schedule.remove(scheduleKey(thing, "satellite"));
    m_schedule.remove(scheduleKey(thing, "ensemble"));
    m_siteForecastAt.remove(thing->id());

    if (myThings().isEmpty() && m_pluginTimer) {
        hardwareManager()->pluginTimerManager()->unregisterTimer(m_pluginTimer);
        m_pluginTimer = nullptr;
    }
}

void IntegrationPluginOpenMeteo::refresh(Thing *thing, bool force)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // A site without valid coordinates asks nothing, and neither do its planes. This holds
    // for a thing whose setup failed but which nymea keeps, e.g. after a reconfigure
    // without coordinates. The staleness checks below still run.
    double lat, lon;
    const bool located = resolveCoordinates(thing, lat, lon);
    if (thing->thingClassId() == openMeteoSiteThingClassId) {
        if (located && (force || m_schedule.due(scheduleKey(thing, "forecast"), now)))
            startForecastPass(thing);
        if (located && (force || m_schedule.due(scheduleKey(thing, "satellite"), now)))
            updateSiteSatellite(thing);

        // Staleness is evaluated on every tick, not only on a successful reply: a
        // forecast that stopped arriving must stop claiming to be valid.
        const qint64 last = m_siteForecastAt.value(thing->id());
        const bool valid = last > 0 && now - last < 3 * ForecastIntervalS;
        thing->setStateValue(openMeteoSiteForecastValidStateTypeId, valid);

    } else if (thing->thingClassId() == openMeteoPvPlaneThingClassId) {
        // The hourly forecast is driven by the parent site (startForecastPass), so that
        // every plane of a site is sampled at the same instant. Only a forced refresh
        // comes through here - initial setup, and the manual action - and it does not
        // move the site's schedule.
        if (located && force)
            updatePlaneForecast(thing, configuredModel(), true, Done());
        if (located && (force || m_schedule.due(scheduleKey(thing, "satellite"), now)))
            updatePlaneSatellite(thing);
        if (located && (force || m_schedule.due(scheduleKey(thing, "ensemble"), now)))
            updatePlaneEnsemble(thing);

        // Like forecastValid, on every tick: a spread that stopped arriving must stop
        // being published as current. Read from the cached state, so a restart neither
        // revives a stale spread nor forgets a fresh one.
        const qint64 spreadTime = thing->stateValue(openMeteoPvPlaneForecastWeatherSpreadTimeStateTypeId).toLongLong();
        if (!EnsembleSpread::fresh(spreadTime, now)) {
            thing->setStateValue(openMeteoPvPlaneForecastWeatherSpreadValidStateTypeId, false);
            invalidateSpread(thing, openMeteoPvPlaneForecastWeatherSpreadTodayPercentStateTypeId);
            invalidateSpread(thing, openMeteoPvPlaneForecastWeatherSpreadTomorrowPercentStateTypeId);
        }
    }
}

void IntegrationPluginOpenMeteo::invalidateSpread(Thing *thing, const StateTypeId &stateTypeId)
{
    // NaN != NaN: setting it again would notify a change at every tick.
    if (!qIsNaN(thing->stateValue(stateTypeId).toDouble()))
        thing->setStateValue(stateTypeId, EnsembleSpread::invalid());
}

void IntegrationPluginOpenMeteo::startForecastPass(Thing *site)
{
    // A site drives the hourly forecast of ALL its planes, in the same pass. They must
    // share one instant: a consumer that sums the planes of a site can only sum curves
    // from the same run. Gated independently, each plane carried its own last-refresh
    // time and re-rolled its own jitter, so two planes drifted apart within minutes - and
    // at every run publication one picked the new run up to an hour before the others,
    // leaving the sum blocked or half stale.
    //
    // For the same reason the pass is retried as a whole: a plane retried alone could
    // pick up a newer run than its siblings.
    QList<Thing *> planes;
    foreach (Thing *child, myThings()) {
        if (child->thingClassId() == openMeteoPvPlaneThingClassId && child->parentId() == site->id())
            planes.append(child);
    }

    const QString key = scheduleKey(site, "forecast");
    m_schedule.started(key, ForecastIntervalS, QDateTime::currentSecsSinceEpoch());

    struct Pass { int pending; bool ok; };
    QSharedPointer<Pass> pass(new Pass{1 + static_cast<int>(planes.count()), true});
    const QString siteName = site->name();
    const Done done = [this, key, pass, siteName](bool ok) {
        pass->ok = pass->ok && ok;
        if (--pass->pending > 0)
            return;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (pass->ok) {
            m_schedule.succeeded(key, ForecastIntervalS, now);
        } else {
            m_schedule.failed(key, ForecastIntervalS, now);
            qCWarning(dcOpenMeteo()) << "Forecast pass failed for" << siteName << "- attempt"
                                     << m_schedule.failures(key) << ", next in"
                                     << m_schedule.nextAttempt(key) - now << "s";
        }
    };

    updateSiteForecast(site, configuredModel(), true, done);
    foreach (Thing *plane, planes)
        updatePlaneForecast(plane, configuredModel(), true, done);
}

Thing *IntegrationPluginOpenMeteo::siteOf(Thing *thing) const
{
    if (thing->thingClassId() == openMeteoSiteThingClassId)
        return thing;
    if (thing->thingClassId() == openMeteoPvPlaneThingClassId)
        return myThings().findById(thing->parentId());
    return nullptr;
}

bool IntegrationPluginOpenMeteo::resolveCoordinates(Thing *thing, double &latitude, double &longitude) const
{
    Thing *site = siteOf(thing);
    if (!site)
        return false;

    // Absent, sentinel or out of range: no coordinates, so no request (CONTRACT_A sec. 4).
    return SiteCoordinates::read(site->paramValue(openMeteoSiteThingLatitudeParamTypeId),
                                 site->paramValue(openMeteoSiteThingLongitudeParamTypeId),
                                 latitude, longitude);
}

void IntegrationPluginOpenMeteo::updateSiteForecast(Thing *thing, const QString &model, bool allowFallback, Done done)
{
    const Done finish = [done](bool ok) { if (done) done(ok); };
    double lat, lon;
    if (!resolveCoordinates(thing, lat, lon)) {
        finish(false);
        return;
    }

    const QString baseUrl = configValue(openMeteoPluginBaseUrlParamTypeId).toString();

    QUrl url(baseUrl + "/v1/forecast");
    QUrlQuery query;
    addCommonQuery(query, lat, lon);
    addElevation(query, thing);
    query.addQueryItem("models", model);
    query.addQueryItem("wind_speed_unit", "ms");
    query.addQueryItem("current", "temperature_2m,wind_speed_10m,wind_direction_10m,cloud_cover,"
                                  "relative_humidity_2m,surface_pressure,weather_code,snowfall,is_day,"
                                  "shortwave_radiation,direct_normal_irradiance,diffuse_radiation,"
                                  "terrestrial_radiation");
    // Hourly GHI and top-of-atmosphere radiation feed the clearness index behind
    // forecastTendencyToday. CONTRACT_A sec. 2.4: shortwave_radiation_clear_sky returns
    // no non-null value at all on the MeteoFrance models, so terrestrial_radiation is
    // the only reference available across the whole horizon.
    //
    // weather_code, is_day and temperature_2m feed forecastHourlyWeather, for the app
    // (since etm16): same request, same host, nothing more sent.
    query.addQueryItem("hourly", "shortwave_radiation,terrestrial_radiation,weather_code,is_day,temperature_2m");
    query.addQueryItem("daily", "temperature_2m_max,temperature_2m_min,sunrise,sunset,shortwave_radiation_sum");
    query.addQueryItem("forecast_days", "2");
    url.setQuery(query);

    qCDebug(dcOpenMeteo()) << "Requesting forecast for" << thing->name() << url.toString();

    QNetworkReply *reply = hardwareManager()->networkManager()->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    connect(reply, &QNetworkReply::finished, thing, [this, thing, reply, model, allowFallback, done, finish]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(dcOpenMeteo()) << "Forecast reply error:" << reply->errorString();
            finish(false);
            return;
        }
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError) {
            qCWarning(dcOpenMeteo()) << "Forecast parse error:" << error.errorString();
            finish(false);
            return;
        }

        const QVariantMap map = doc.toVariant().toMap();
        if (map.value("error").toBool()) {
            qCWarning(dcOpenMeteo()) << "Forecast API error:" << map.value("reason").toString();
            finish(false);
            return;
        }

        const QVariantMap current = map.value("current").toMap();
        const QVariantMap daily = map.value("daily").toMap();
        const QVariantMap hourly = map.value("hourly").toMap();
        const int utcOffset = map.value("utc_offset_seconds").toInt();

        // CONTRACT_A sec. 3.2. A model that does not serve radiation answers HTTP 200
        // with nulls, not an error - meteofrance_arome_france_hd serves none at all.
        // Fall back, and let forecastModel say which model actually answered: MeteoFrance
        // without irradiance is not a degraded version of the same service.
        if (radiationAllNull(current, hourly, {"shortwave_radiation"})) {
            if (allowFallback && model != QLatin1String(FallbackModel)) {
                qCWarning(dcOpenMeteo()) << "Model" << model << "returned no radiation at all. Falling back to"
                                         << FallbackModel << "for" << thing->name();
                updateSiteForecast(thing, QString::fromLatin1(FallbackModel), false, done);
            } else {
                // A 200 that carries nothing is a failure, retried as one (CONTRACT_A sec. 3.1).
                qCWarning(dcOpenMeteo()) << "No radiation from" << model << "and no fallback left for" << thing->name();
                finish(false);
            }
            return;
        }

        if (current.contains("time"))
            thing->setStateValue(openMeteoSiteUpdateTimeStateTypeId, current.value("time").toUInt());
        if (current.contains("temperature_2m"))
            thing->setStateValue(openMeteoSiteTemperatureStateTypeId, current.value("temperature_2m").toDouble());
        if (current.contains("wind_speed_10m"))
            thing->setStateValue(openMeteoSiteWindSpeedStateTypeId, current.value("wind_speed_10m").toDouble());
        if (current.contains("wind_direction_10m"))
            thing->setStateValue(openMeteoSiteWindDirectionStateTypeId, current.value("wind_direction_10m").toInt());
        if (current.contains("cloud_cover"))
            thing->setStateValue(openMeteoSiteCloudinessStateTypeId, current.value("cloud_cover").toInt());
        if (current.contains("relative_humidity_2m"))
            thing->setStateValue(openMeteoSiteHumidityStateTypeId, current.value("relative_humidity_2m").toInt());
        if (current.contains("surface_pressure"))
            thing->setStateValue(openMeteoSitePressureStateTypeId, current.value("surface_pressure").toDouble());
        if (current.contains("snowfall"))
            thing->setStateValue(openMeteoSiteSnowfallStateTypeId, current.value("snowfall").toDouble());
        if (current.contains("shortwave_radiation"))
            thing->setStateValue(openMeteoSiteForecastGhiStateTypeId, current.value("shortwave_radiation").toDouble());
        if (current.contains("direct_normal_irradiance"))
            thing->setStateValue(openMeteoSiteForecastDniStateTypeId, current.value("direct_normal_irradiance").toDouble());
        if (current.contains("diffuse_radiation"))
            thing->setStateValue(openMeteoSiteForecastDhiStateTypeId, current.value("diffuse_radiation").toDouble());
        if (current.contains("terrestrial_radiation"))
            thing->setStateValue(openMeteoSiteTerrestrialRadiationStateTypeId, current.value("terrestrial_radiation").toDouble());

        bool isDay = current.value("is_day").toInt() == 1;
        if (current.contains("is_day"))
            thing->setStateValue(openMeteoSiteDaylightStateTypeId, isDay);
        if (current.contains("weather_code")) {
            int weatherCode = current.value("weather_code").toInt();
            thing->setStateValue(openMeteoSiteWeatherConditionStateTypeId,
                                 weatherConditionFromCode(weatherCode, isDay));
            thing->setStateValue(openMeteoSiteWeatherDescriptionStateTypeId,
                                 weatherDescriptionFromCode(weatherCode));
        }

        // Reached only when this model actually served radiation, so the value published
        // is a provenance and not an intention.
        thing->setStateValue(openMeteoSiteForecastModelStateTypeId, model);

        const QVariantList tmax = daily.value("temperature_2m_max").toList();
        const QVariantList tmin = daily.value("temperature_2m_min").toList();
        const QVariantList sunrise = daily.value("sunrise").toList();
        const QVariantList sunset = daily.value("sunset").toList();
        const QVariantList swSum = daily.value("shortwave_radiation_sum").toList();
        if (!tmax.isEmpty())
            thing->setStateValue(openMeteoSiteTemperatureMaxStateTypeId, tmax.first().toDouble());
        if (!tmin.isEmpty())
            thing->setStateValue(openMeteoSiteTemperatureMinStateTypeId, tmin.first().toDouble());
        if (!sunrise.isEmpty())
            thing->setStateValue(openMeteoSiteSunriseTimeStateTypeId, sunrise.first().toUInt());
        if (!sunset.isEmpty())
            thing->setStateValue(openMeteoSiteSunsetTimeStateTypeId, sunset.first().toUInt());
        if (!swSum.isEmpty())
            // Open-Meteo shortwave_radiation_sum is in MJ/m^2; expose kWh/m^2 (/ 3.6).
            thing->setStateValue(openMeteoSiteForecastGhiSumTodayStateTypeId, swSum.first().toDouble() / 3.6);

        // Clearness index over today's hours -> sky tendency. Descriptive label only:
        // if a consumer starts weighting decisions with it, the logic belongs in
        // etm-powersync-optimizer (CLAUDE.md license boundary).
        const QVariantList hTimes = hourly.value("time").toList();
        const QVariantList hGhi = hourly.value("shortwave_radiation").toList();
        const QVariantList hToa = hourly.value("terrestrial_radiation").toList();
        if (!hTimes.isEmpty() && hGhi.count() == hTimes.count() && hToa.count() == hTimes.count()) {
            const uint now = current.value("time").toUInt();
            const uint dayStart = PvCurve::localDayStart(now, utcOffset);
            double ghiSum = 0, toaSum = 0;
            for (int i = 0; i < hTimes.count(); i++) {
                const uint t = hTimes.at(i).toUInt();
                if (t <= dayStart || t > dayStart + 86400)
                    continue;
                if (hGhi.at(i).isNull() || hToa.at(i).isNull())
                    continue;
                ghiSum += hGhi.at(i).toDouble();
                toaSum += hToa.at(i).toDouble();
            }
            QString tendency = "unknown";
            if (toaSum > 0) {
                const double kt = ghiSum / toaSum;
                tendency = kt > 0.55 ? "sunny" : (kt >= 0.25 ? "mixed" : "overcast");
            }
            thing->setStateValue(openMeteoSiteForecastTendencyTodayStateTypeId, tendency);
        }

        m_siteForecastAt.insert(thing->id(), QDateTime::currentSecsSinceEpoch());
        thing->setStateValue(openMeteoSiteForecastValidStateTypeId, true);
        finish(true);

        // Hourly weather of the site for the app (CONTRACT_A sec. 1.8). Not a failure of
        // the forecast when it is missing: the radiation, which ems needs, did arrive.
        const QVariantMap weather = HourlySeries::siteWeather(map);
        if (weather.isEmpty())
            qCWarning(dcOpenMeteo()) << "No hourly weather in the forecast reply for" << thing->name();

        // Provenance of the run, for the model that actually answered. The hourly weather
        // carries it inside, like the transport curve, so it is set in the continuation.
        withRunProvenance(thing, model, [thing, weather, model](uint issued, uint available) {
            thing->setStateValue(openMeteoSiteForecastIssuedAtStateTypeId, issued);
            thing->setStateValue(openMeteoSiteForecastAvailableAtStateTypeId, available);
            if (!weather.isEmpty()) {
                QVariantMap series = weather;
                series.insert("model", model);
                series.insert("issuedAt", issued);
                thing->setStateValue(openMeteoSiteForecastHourlyWeatherStateTypeId, series);
            }
        });
    });
}

QString IntegrationPluginOpenMeteo::provenanceDomain(const QString &model)
{
    // Only real grid domains serve meta.json; composite names answer HTTP 500 (measured
    // 2026-09-08: meteofrance_seamless, best_match, icon_seamless, icon_eu all 500).
    //
    // meteofrance_seamless serves the first 60 h from AROME and the rest from ARPEGE.
    // The AROME run is the one that matters for a today/J+1 curve, and both AROME grids
    // (0.01 and 0.025) come from the same NWP run, so the HD domain carries that run
    // identity. The far horizon comes from a different, older ARPEGE run - stated in
    // CONTRACT_A sec. 3.3 rather than glossed over.
    if (model == QLatin1String("meteofrance_seamless"))
        return QStringLiteral("meteofrance_arome_france_hd");

    // best_match is resolved per location by the server. There is no single honest run
    // identity for it, so we publish none rather than a plausible guess.
    if (model == QLatin1String("best_match"))
        return QString();

    return model;
}

void IntegrationPluginOpenMeteo::withRunProvenance(Thing *thing, const QString &model,
                                                   std::function<void(uint, uint)> continuation)
{
    const QString domain = provenanceDomain(model);
    if (domain.isEmpty()) {
        qCDebug(dcOpenMeteo()) << "No single run identity for model" << model << "- publishing none.";
        continuation(0, 0);
        return;
    }

    const QString baseUrl = configValue(openMeteoPluginBaseUrlParamTypeId).toString();
    QUrl url(baseUrl + "/data/" + domain + "/static/meta.json");

    QNetworkRequest request(url);
    const QByteArray etag = m_metaEtags.value(domain);
    if (!etag.isEmpty())
        request.setRawHeader("If-None-Match", etag);

    qCDebug(dcOpenMeteo()) << "Requesting run provenance for" << domain << url.toString();

    QNetworkReply *reply = hardwareManager()->networkManager()->get(request);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    connect(reply, &QNetworkReply::finished, thing, [this, reply, domain, continuation]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (status == 304) {
            // Same run as last time: reuse what we already read, no payload transferred.
            const QPair<uint, uint> times = m_metaTimes.value(domain);
            continuation(times.first, times.second);
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            // CONTRACT_A sec. 3.4: no key rather than a wrong one. A consumer must skip
            // the revision, never substitute its own clock - the gap between run
            // initialisation and reception is 4 to 9 hours.
            qCWarning(dcOpenMeteo()) << "Run provenance unavailable for" << domain << ":" << reply->errorString();
            continuation(0, 0);
            return;
        }

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        const QVariantMap meta = doc.toVariant().toMap();
        if (error.error != QJsonParseError::NoError || meta.value("error").toBool()
                || !meta.contains("last_run_initialisation_time")) {
            qCWarning(dcOpenMeteo()) << "Run provenance unreadable for" << domain;
            continuation(0, 0);
            return;
        }

        const uint issued = meta.value("last_run_initialisation_time").toUInt();
        const uint available = meta.value("last_run_availability_time").toUInt();
        m_metaTimes.insert(domain, qMakePair(issued, available));
        const QByteArray newEtag = reply->rawHeader("ETag");
        if (!newEtag.isEmpty())
            m_metaEtags.insert(domain, newEtag);

        continuation(issued, available);
    });
}

void IntegrationPluginOpenMeteo::updateSiteSatellite(Thing *thing)
{
    double lat, lon;
    if (!resolveCoordinates(thing, lat, lon))
        return;

    const QString key = scheduleKey(thing, "satellite");
    m_schedule.started(key, SatelliteIntervalS, QDateTime::currentSecsSinceEpoch());
    const auto fail = [this, key]() {
        m_schedule.failed(key, SatelliteIntervalS, QDateTime::currentSecsSinceEpoch());
    };

    const QString satelliteBaseUrl = configValue(openMeteoPluginSatelliteBaseUrlParamTypeId).toString();

    // Observation product (geostationary satellite), not a forecast.
    //
    // The satellite endpoint silently ignores "current=": it answers HTTP 200 with no
    // error field and simply no "current" block, so the observed states were never set.
    // Measured 2026-09-08. Use hourly + past_days and take the last non-null sample;
    // measured latency is about 59 min, which is why observedTime is published.
    QUrl url(satelliteBaseUrl + "/v1/archive");
    QUrlQuery query;
    addCommonQuery(query, lat, lon);
    query.addQueryItem("models", "satellite_radiation_seamless");
    query.addQueryItem("hourly", "shortwave_radiation,shortwave_radiation_clear_sky");
    query.addQueryItem("past_days", "1");
    query.addQueryItem("forecast_days", "1");
    url.setQuery(query);

    qCDebug(dcOpenMeteo()) << "Requesting satellite observation for" << thing->name() << url.toString();

    QNetworkReply *reply = hardwareManager()->networkManager()->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    connect(reply, &QNetworkReply::finished, thing, [this, thing, reply, key, fail]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(dcOpenMeteo()) << "Satellite reply error:" << reply->errorString();
            fail();
            return;
        }
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError) {
            qCWarning(dcOpenMeteo()) << "Satellite parse error:" << error.errorString();
            fail();
            return;
        }

        const QVariantMap map = doc.toVariant().toMap();
        if (map.value("error").toBool()) {
            qCWarning(dcOpenMeteo()) << "Satellite API error:" << map.value("reason").toString();
            fail();
            return;
        }

        const QVariantMap hourly = map.value("hourly").toMap();
        const QVariantList times = hourly.value("time").toList();
        const QVariantList ghi = hourly.value("shortwave_radiation").toList();
        const QVariantList clear = hourly.value("shortwave_radiation_clear_sky").toList();
        if (times.isEmpty() || ghi.count() != times.count()) {
            qCWarning(dcOpenMeteo()) << "Satellite reply carried no usable series";
            fail();
            return;
        }

        int last = -1;
        for (int i = times.count() - 1; i >= 0; i--) {
            if (!ghi.at(i).isNull()) {
                last = i;
                break;
            }
        }
        if (last < 0) {
            qCWarning(dcOpenMeteo()) << "Satellite reply carried no non-null sample";
            fail();
            return;
        }

        const double ghiObs = ghi.at(last).toDouble();
        thing->setStateValue(openMeteoSiteObservedGhiStateTypeId, ghiObs);
        thing->setStateValue(openMeteoSiteObservedTimeStateTypeId, times.at(last).toUInt());

        // Unlike the forecast API, the satellite product does carry a real clear-sky
        // series (CONTRACT_A sec. 2.4 applies to the forecast models only).
        if (last < clear.count() && !clear.at(last).isNull()) {
            const double clearSky = clear.at(last).toDouble();
            thing->setStateValue(openMeteoSiteObservedClearSkyGhiStateTypeId, clearSky);
            // kt = observed / clear-sky, with a night guard: near sunrise and sunset the
            // denominator collapses and the ratio becomes meaningless.
            double kt = 0.0;
            if (clearSky > 20.0)
                kt = qBound(0.0, ghiObs / clearSky, 1.2);
            thing->setStateValue(openMeteoSiteObservedClearSkyIndexStateTypeId, kt);
        }

        m_schedule.succeeded(key, SatelliteIntervalS, QDateTime::currentSecsSinceEpoch());
    });
}

void IntegrationPluginOpenMeteo::updatePlaneForecast(Thing *thing, const QString &model, bool allowFallback, Done done)
{
    const Done finish = [done](bool ok) { if (done) done(ok); };
    double lat, lon;
    if (!resolveCoordinates(thing, lat, lon)) {
        finish(false);
        return;
    }

    const QString baseUrl = configValue(openMeteoPluginBaseUrlParamTypeId).toString();

    const double tilt = thing->paramValue(openMeteoPvPlaneThingTiltParamTypeId).toDouble();
    const double azimuthOm = azimuthToOpenMeteo(thing->paramValue(openMeteoPvPlaneThingAzimuthParamTypeId).toDouble());

    QUrl url(baseUrl + "/v1/forecast");
    QUrlQuery query;
    addCommonQuery(query, lat, lon);
    // Since etm16 the plane sends the site's elevation too, so its temperature_2m - which
    // corrects the curve - is read at the declared altitude, like the site's, and not at
    // the altitude of the model's terrain. GTI does not depend on it (measured 2026-10-15:
    // identical on all 72 samples; temperature_2m +0.0 to +0.1 C for 153 m vs 156 m).
    addElevation(query, myThings().findById(thing->parentId()));
    query.addQueryItem("models", model);
    query.addQueryItem("tilt", QString::number(tilt));
    query.addQueryItem("azimuth", QString::number(azimuthOm));
    // "current" is instantaneous; "hourly" is the preceding-hour mean. Both are needed:
    // the instant drives forecastPvPower, the hourly series drives every integral.
    // temperature_2m rides on the same request and the same time grid: it feeds the
    // temperature correction of the curve and of the scalar states, and is published raw
    // in the curve.
    query.addQueryItem("current", "global_tilted_irradiance,temperature_2m");
    query.addQueryItem("hourly", "global_tilted_irradiance,temperature_2m");
    query.addQueryItem("forecast_days", "3");
    url.setQuery(query);

    qCDebug(dcOpenMeteo()) << "Requesting GTI for plane" << thing->name() << url.toString();

    QNetworkReply *reply = hardwareManager()->networkManager()->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    connect(reply, &QNetworkReply::finished, thing, [this, thing, reply, model, allowFallback, done, finish]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(dcOpenMeteo()) << "GTI reply error:" << reply->errorString();
            finish(false);
            return;
        }
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError) {
            qCWarning(dcOpenMeteo()) << "GTI parse error:" << error.errorString();
            finish(false);
            return;
        }

        const QVariantMap map = doc.toVariant().toMap();
        if (map.value("error").toBool()) {
            qCWarning(dcOpenMeteo()) << "GTI API error:" << map.value("reason").toString();
            finish(false);
            return;
        }

        const QVariantMap current = map.value("current").toMap();
        const QVariantMap hourly = map.value("hourly").toMap();
        const int utcOffset = map.value("utc_offset_seconds").toInt();

        // Same fallback as the site (CONTRACT_A sec. 3.2): a model with no GTI answers
        // 200 with nulls. The plane must follow the site, or the two would report the
        // same site through two different models without saying so.
        if (radiationAllNull(current, hourly, {"global_tilted_irradiance"})) {
            if (allowFallback && model != QLatin1String(FallbackModel)) {
                qCWarning(dcOpenMeteo()) << "Model" << model << "returned no GTI. Falling back to"
                                         << FallbackModel << "for plane" << thing->name();
                updatePlaneForecast(thing, QString::fromLatin1(FallbackModel), false, done);
            } else {
                qCWarning(dcOpenMeteo()) << "No GTI from" << model << "and no fallback left for plane" << thing->name();
                finish(false);
            }
            return;
        }

        // CONTRACT_A sec. 2.1: power is a rule of three on the API value and nothing
        // else. performanceRatio defaults to 1.00 so no coefficient hides inside a
        // number presented as a relay; below 1 it is a derating the installer declared.
        PvCurve::Plane plane;
        plane.kwp = thing->paramValue(openMeteoPvPlaneThingKwpParamTypeId).toDouble();
        plane.performanceRatio = thing->paramValue(openMeteoPvPlaneThingPerformanceRatioParamTypeId).toDouble();
        plane.tempCoeffPctPerK = thing->paramValue(openMeteoPvPlaneThingTempCoeffPctPerKParamTypeId).toDouble();
        plane.noctC = thing->paramValue(openMeteoPvPlaneThingNoctCParamTypeId).toDouble();

        const QVariantList times = hourly.value("time").toList();
        const QVariantList gtis = hourly.value("global_tilted_irradiance").toList();
        const QVariantList temperatures = hourly.value("temperature_2m").toList();
        const uint now = current.value("time").toUInt();
        const uint todayStart = PvCurve::localDayStart(now, utcOffset);

        // The scalar states follow the decision of the run, the same one as the curve
        // below: corrected for module temperature together, or not at all (CONTRACT_A
        // sec. 2.6). Their energies are sums of the same rounded powers as p.
        const bool corrected = PvCurve::temperatureComplete(times, temperatures, todayStart);
        Thing *site = myThings().findById(thing->parentId());
        const uint sunset = site ? site->stateValue(openMeteoSiteSunsetTimeStateTypeId).toUInt() : 0;
        const QVariantMap scalars = PvCurve::scalars(map, todayStart, sunset, plane, corrected);
        if (corrected && current.contains("global_tilted_irradiance") && !scalars.contains("pvPower"))
            qCWarning(dcOpenMeteo()) << "No current temperature_2m for plane" << thing->name()
                                     << "- forecastPvPower not updated rather than published uncorrected.";

        const struct { const char *key; StateTypeId id; } published[] = {
            { "gtiSource",       openMeteoPvPlaneForecastGtiSourceStateTypeId },
            { "pvPower",         openMeteoPvPlaneForecastPvPowerStateTypeId },
            { "energyNext3h",    openMeteoPvPlaneForecastEnergyNext3hStateTypeId },
            { "energyNext6h",    openMeteoPvPlaneForecastEnergyNext6hStateTypeId },
            { "energyNext12h",   openMeteoPvPlaneForecastEnergyNext12hStateTypeId },
            { "energyRestOfDay", openMeteoPvPlaneForecastEnergyRestOfDayStateTypeId },
            { "energyTomorrow",  openMeteoPvPlaneForecastEnergyTomorrowStateTypeId },
            { "peakTime",        openMeteoPvPlaneForecastPeakTimeStateTypeId },
        };
        for (const auto &state : published) {
            if (scalars.contains(state.key))
                thing->setStateValue(state.id, scalars.value(state.key));
        }
        if (current.contains("time"))
            thing->setStateValue(openMeteoPvPlaneUpdateTimeStateTypeId, current.value("time").toUInt());

        if (times.isEmpty() || gtis.count() != times.count() || PvCurve::firstForwardBucket(times, now) < 0) {
            qCWarning(dcOpenMeteo()) << "GTI reply for plane" << thing->name() << "carried no usable series";
            finish(false);
            return;
        }

        // ---- Transport channel (CONTRACT_A sec. 5) ------------------------------
        // Whole days, today 00:00 local through tomorrow 23:00: 48 slots, past hours
        // included. Fixed day boundaries are what make two revisions of the same day
        // comparable slot by slot; a rolling "future only" window would shrink through
        // the day and no two revisions would line up. The elapsed hours are also what a
        // consumer needs to draw the forecast against measured production.
        //
        // p[i] is the mean power over [t0 + i*step, t0 + (i+1)*step). The API value
        // timestamped T is the mean over (T-3600, T], so slot i reads the sample at
        // t0 + (i+1)*3600 - the same off-by-one this contract is built around, stated in
        // the payload itself rather than left to be rediscovered.
        //
        // v2 (CONTRACT_A sec. 5.9): p is corrected for module temperature with the
        // plane's declared coefficient and NOCT, for the whole run or not at all, and
        // the raw GTI and temperature_2m travel beside it for the record. ems reads v1
        // and v2 since 1.15.2+etm66; it must be deployed before this producer.
        QVariantMap hourlyCurve;
        const bool complete = PvCurve::build(times, gtis, temperatures, todayStart, plane, hourlyCurve);

        if (complete) {
            if (hourlyCurve.value("corrections").toList().isEmpty())
                qCWarning(dcOpenMeteo()) << "No complete temperature_2m series for plane" << thing->name()
                                         << "- curve published without temperature correction.";
            // The curve is built inside the provenance continuation, never from the
            // site's state. Reading the site raced the asynchronous meta.json round trip
            // and embedded the PREVIOUS run's key - observed in testing,
            // wrong by a whole AROME run (3 h). A revision keyed on the wrong run is
            // exactly what CONTRACT_A sec. 3.3 exists to prevent.
            withRunProvenance(thing, model, [thing, hourlyCurve, model](uint issued, uint) {
                QVariantMap curve = hourlyCurve;
                // Provenance travels with the payload so a stored revision carries its
                // own model and approximate age, with no second lookup. issuedAt 0 means
                // unknown: store the age as NULL, never substitute an instant (sec. 3.3).
                curve.insert("model", model);
                curve.insert("issuedAt", issued);
                // setStateValue discards an unchanged value, so repeated cycles inside a
                // single run are neither stored nor notified.
                thing->setStateValue(openMeteoPvPlaneForecastCurveTransportStateTypeId, curve);
            });
        } else {
            qCWarning(dcOpenMeteo()) << "Incomplete 48 h window for plane" << thing->name() << "- curve not published.";
        }

        // The pass succeeds only if the curve - what ems stores - was refreshed.
        finish(complete);
    });
}

void IntegrationPluginOpenMeteo::updatePlaneSatellite(Thing *thing)
{
    double lat, lon;
    if (!resolveCoordinates(thing, lat, lon))
        return;

    const QString key = scheduleKey(thing, "satellite");
    m_schedule.started(key, SatelliteIntervalS, QDateTime::currentSecsSinceEpoch());
    const auto fail = [this, key]() {
        m_schedule.failed(key, SatelliteIntervalS, QDateTime::currentSecsSinceEpoch());
    };

    const QString satelliteBaseUrl = configValue(openMeteoPluginSatelliteBaseUrlParamTypeId).toString();
    const double tilt = thing->paramValue(openMeteoPvPlaneThingTiltParamTypeId).toDouble();
    const double azimuthOm = azimuthToOpenMeteo(thing->paramValue(openMeteoPvPlaneThingAzimuthParamTypeId).toDouble());

    // The satellite product also serves global_tilted_irradiance for a given tilt and
    // azimuth, so the plane gets an observed GTI next to its forecast one. No observed
    // PV power is derived from it - see MAPPING sec. 4.1: that state would sit next to a
    // real inverter measurement and invite the forecast-vs-measured comparison that
    // CONTRACT_A sec. 0.2 forbids, because that comparison is learning.
    QUrl url(satelliteBaseUrl + "/v1/archive");
    QUrlQuery query;
    addCommonQuery(query, lat, lon);
    query.addQueryItem("models", "satellite_radiation_seamless");
    query.addQueryItem("tilt", QString::number(tilt));
    query.addQueryItem("azimuth", QString::number(azimuthOm));
    query.addQueryItem("hourly", "global_tilted_irradiance");
    query.addQueryItem("past_days", "1");
    query.addQueryItem("forecast_days", "1");
    url.setQuery(query);

    qCDebug(dcOpenMeteo()) << "Requesting satellite GTI for plane" << thing->name() << url.toString();

    QNetworkReply *reply = hardwareManager()->networkManager()->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    connect(reply, &QNetworkReply::finished, thing, [this, thing, reply, key, fail]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(dcOpenMeteo()) << "Satellite GTI reply error:" << reply->errorString();
            fail();
            return;
        }
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError) {
            qCWarning(dcOpenMeteo()) << "Satellite GTI parse error:" << error.errorString();
            fail();
            return;
        }

        const QVariantMap map = doc.toVariant().toMap();
        if (map.value("error").toBool()) {
            qCWarning(dcOpenMeteo()) << "Satellite GTI API error:" << map.value("reason").toString();
            fail();
            return;
        }

        const QVariantMap hourly = map.value("hourly").toMap();
        const QVariantList times = hourly.value("time").toList();
        const QVariantList gtis = hourly.value("global_tilted_irradiance").toList();
        if (times.isEmpty() || gtis.count() != times.count()) {
            qCWarning(dcOpenMeteo()) << "Satellite GTI reply carried no usable series";
            fail();
            return;
        }

        bool found = false;
        for (int i = times.count() - 1; i >= 0; i--) {
            if (gtis.at(i).isNull())
                continue;
            thing->setStateValue(openMeteoPvPlaneObservedGtiStateTypeId, gtis.at(i).toDouble());
            thing->setStateValue(openMeteoPvPlaneObservedGtiTimeStateTypeId, times.at(i).toUInt());
            found = true;
            break;
        }
        if (!found) {
            qCWarning(dcOpenMeteo()) << "Satellite GTI reply carried no non-null sample";
            fail();
            return;
        }

        // The whole hourly series as well (CONTRACT_A sec. 1.7): observedGti holds the
        // last hour only, and an hour that became available between two polls was lost.
        // observedGti/observedGtiTime are kept as they are - ems etm69 reads them.
        const QVariantMap series = HourlySeries::observedGti(map);
        if (!series.isEmpty())
            thing->setStateValue(openMeteoPvPlaneObservedGtiSeriesStateTypeId, series);

        m_schedule.succeeded(key, SatelliteIntervalS, QDateTime::currentSecsSinceEpoch());
    });
}

void IntegrationPluginOpenMeteo::updatePlaneEnsemble(Thing *thing)
{
    double lat, lon;
    if (!resolveCoordinates(thing, lat, lon))
        return;

    const QString key = scheduleKey(thing, "ensemble");
    m_schedule.started(key, EnsembleIntervalS, QDateTime::currentSecsSinceEpoch());
    const auto fail = [this, key]() {
        m_schedule.failed(key, EnsembleIntervalS, QDateTime::currentSecsSinceEpoch());
    };

    const QString ensembleBaseUrl = configValue(openMeteoPluginEnsembleBaseUrlParamTypeId).toString();
    const QString ensembleModel = configValue(openMeteoPluginEnsembleModelParamTypeId).toString();
    const double tilt = thing->paramValue(openMeteoPvPlaneThingTiltParamTypeId).toDouble();
    const double azimuthOm = azimuthToOpenMeteo(thing->paramValue(openMeteoPvPlaneThingAzimuthParamTypeId).toDouble());

    // Spread lives on the plane and is computed on GTI, not on GHI: measured 2026-09-08,
    // CV(GTI) exceeds CV(GHI) by 2.0 to 4.6 points, systematically, and the gap widens
    // with the spread. GHI would understate the dispersion of the published quantity.
    //
    // No MeteoFrance ensemble exists, so the spread necessarily comes from another model
    // family than the forecast - hence forecastSpreadModel (CONTRACT_A sec. 2.5).
    QUrl url(ensembleBaseUrl + "/v1/ensemble");
    QUrlQuery query;
    addCommonQuery(query, lat, lon);
    query.addQueryItem("models", ensembleModel);
    query.addQueryItem("tilt", QString::number(tilt));
    query.addQueryItem("azimuth", QString::number(azimuthOm));
    query.addQueryItem("hourly", "global_tilted_irradiance");
    query.addQueryItem("forecast_days", "3");
    url.setQuery(query);

    qCDebug(dcOpenMeteo()) << "Requesting ensemble spread for plane" << thing->name() << url.toString();

    QNetworkReply *reply = hardwareManager()->networkManager()->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    connect(reply, &QNetworkReply::finished, thing, [this, thing, reply, ensembleModel, key, fail]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(dcOpenMeteo()) << "Ensemble reply error:" << reply->errorString();
            fail();
            return;
        }
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError) {
            qCWarning(dcOpenMeteo()) << "Ensemble parse error:" << error.errorString();
            fail();
            return;
        }

        const QVariantMap map = doc.toVariant().toMap();
        if (map.value("error").toBool()) {
            qCWarning(dcOpenMeteo()) << "Ensemble API error:" << map.value("reason").toString();
            fail();
            return;
        }

        const QVariantMap hourly = map.value("hourly").toMap();
        const QVariantList times = hourly.value("time").toList();
        if (times.isEmpty()) {
            qCWarning(dcOpenMeteo()) << "Ensemble reply carried no time series";
            fail();
            return;
        }
        const QVariantList spread = EnsembleSpread::daily(map);
        if (spread.isEmpty() || (spread.at(0).isNull() && spread.at(1).isNull())) {
            qCWarning(dcOpenMeteo()) << "Ensemble reply gave no spread for today nor tomorrow";
            fail();
            return;
        }

        // A day that could not be computed goes invalid rather than keeping a value that
        // forecastWeatherSpreadTime would then date as fresh (CONTRACT_A sec. 1.5).
        const StateTypeId days[] = { openMeteoPvPlaneForecastWeatherSpreadTodayPercentStateTypeId,
                                     openMeteoPvPlaneForecastWeatherSpreadTomorrowPercentStateTypeId };
        for (int day = 0; day < 2; day++) {
            if (spread.at(day).isNull())
                invalidateSpread(thing, days[day]);
            else
                thing->setStateValue(days[day], spread.at(day).toDouble());
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        thing->setStateValue(openMeteoPvPlaneForecastWeatherSpreadTimeStateTypeId, now);
        thing->setStateValue(openMeteoPvPlaneForecastWeatherSpreadValidStateTypeId, true);
        thing->setStateValue(openMeteoPvPlaneForecastSpreadModelStateTypeId, ensembleModel);
        m_schedule.succeeded(key, EnsembleIntervalS, now);
    });
}

QString IntegrationPluginOpenMeteo::weatherConditionFromCode(int code, bool isDay) const
{
    // WMO weather interpretation codes -> weatherCondition enum (see metadata possibleValues).
    switch (code) {
    case 0:
        return isDay ? "clear-day" : "clear-night";
    case 1:
    case 2:
        return isDay ? "few-clouds-day" : "few-clouds-night";
    case 3:
        return "overcast";
    case 45:
    case 48:
        return "fog";
    case 51: case 53: case 55: case 56: case 57:
        return "light-rain";
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82:
        return "shower-rain";
    case 71: case 73: case 75: case 77:
    case 85: case 86:
        return "snow";
    case 95: case 96: case 99:
        return "thunderstorm";
    default:
        return "clouds";
    }
}

QString IntegrationPluginOpenMeteo::weatherDescriptionFromCode(int code) const
{
    // WMO weather interpretation codes -> human-readable description.
    switch (code) {
    case 0:                     return tr("Clear sky");
    case 1:                     return tr("Mainly clear");
    case 2:                     return tr("Partly cloudy");
    case 3:                     return tr("Overcast");
    case 45: case 48:           return tr("Fog");
    case 51: case 53: case 55:  return tr("Drizzle");
    case 56: case 57:           return tr("Freezing drizzle");
    case 61: case 63: case 65:  return tr("Rain");
    case 66: case 67:           return tr("Freezing rain");
    case 71: case 73: case 75:  return tr("Snow fall");
    case 77:                    return tr("Snow grains");
    case 80: case 81: case 82:  return tr("Rain showers");
    case 85: case 86:           return tr("Snow showers");
    case 95:                    return tr("Thunderstorm");
    case 96: case 99:           return tr("Thunderstorm with hail");
    default:                    return tr("Unknown");
    }
}
