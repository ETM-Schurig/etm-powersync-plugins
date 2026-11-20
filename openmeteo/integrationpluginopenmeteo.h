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

#ifndef INTEGRATIONPLUGINOPENMETEO_H
#define INTEGRATIONPLUGINOPENMETEO_H

#include <integrations/integrationplugin.h>
#include <plugintimer.h>

#include "ensemblespread.h"
#include "retryschedule.h"

#include <QDateTime>
#include <QHash>
#include <QUrlQuery>
#include <QVariantList>
#include <functional>

class IntegrationPluginOpenMeteo : public IntegrationPlugin
{
    Q_OBJECT

    Q_PLUGIN_METADATA(IID "io.nymea.IntegrationPlugin" FILE "integrationpluginopenmeteo.json")
    Q_INTERFACES(IntegrationPlugin)

public:
    explicit IntegrationPluginOpenMeteo();

    void init() override;
    // A PV plane belongs to a site: it reads its coordinates from the parent. A
    // user-created thing has no way to name its parent (Integrations.AddThing carries no
    // parentId), so planes are discovered instead - one descriptor per configured site,
    // each carrying parentId. nymea applies a descriptor's parentId when creating the
    // thing (thingmanagerimplementation.cpp:375).
    void discoverThings(ThingDiscoveryInfo *info) override;
    void setupThing(ThingSetupInfo *info) override;
    void executeAction(ThingActionInfo *info) override;
    void thingRemoved(Thing *thing) override;

private:
    PluginTimer *m_pluginTimer = nullptr;

    // Per-source scheduling. None of the three upstream products refreshes every minute,
    // so each is gated on its own interval (see docs/MAPPING_openmeteo.md sec. 6):
    // forecast 1 h (runs every 3-6 h, published 4-5 h after init), satellite 1 h
    // (measured latency ~59 min), ensemble 3 h (runs every 6 h).
    //
    // The timer ticks every MINUTE, not every 15. With a 900 s tick the +/-60 s jitter
    // could only choose between the 60 min and the 75 min tick: the mean interval was
    // 67 min rather than 60 (measured overnight in testing), and - the real defect -
    // a whole fleet stayed aligned on the same quarter-hour grid, so the jitter
    // de-correlated almost nothing. Jitter exists to spread a fleet across the API; it
    // needs a resolution finer than the spread it is meant to produce.
    static const int ForecastIntervalS = 3600;
    static const int SatelliteIntervalS = 3600;
    static const int EnsembleIntervalS = EnsembleSpread::IntervalS;

    // Since etm16 a failed source is retried after 5 min x 2^(n-1), capped at its interval,
    // instead of at every tick (CONTRACT_A sec. 3.1). Keys: "<thingId>/<source>".
    RetrySchedule m_schedule;
    static QString scheduleKey(Thing *thing, const char *source);
    // Last success of the site's own forecast reply, for forecastValid. The forecast pass
    // of a site (site + all its planes) is scheduled as one source in m_schedule.
    QHash<ThingId, qint64> m_siteForecastAt;

    // Called once per request with its outcome, after any model fallback.
    using Done = std::function<void(bool ok)>;

    // The hourly forecast of a site and of all its planes, sent together (CONTRACT_A
    // sec. 5.7) and scheduled as one source: it succeeds when every reply succeeded, and
    // a failure of any retries them all, so the planes keep sharing one instant.
    void startForecastPass(Thing *site);

    // Refresh dispatch. force=true bypasses the per-source intervals (manual action).
    void refresh(Thing *thing, bool force = false);

    // Site (parent): weather forecast + satellite observation.
    // The model is passed explicitly so the fallback can re-issue the same request
    // against best_match. allowFallback guards against looping.
    void updateSiteForecast(Thing *thing, const QString &model, bool allowFallback, Done done);
    void updateSiteSatellite(Thing *thing);

    // Run provenance, from {baseUrl}/data/<model>/static/meta.json. Issued at run
    // INITIALISATION - when the run describes the world - which is the revision key.
    // Availability is when it became readable, 4 to 9 hours later depending on the model
    // (measured 2026-09-08). Confusing the two would misdate every revision by that much,
    // and both numbers look plausible.
    // Resolves the run provenance for a model and hands it to the continuation.
    // The curve must be built INSIDE that continuation: reading the site's state instead
    // races the asynchronous meta.json round trip, and the plane then embeds the previous
    // run's key - observed in testing, off by a whole run (3 h).
    void withRunProvenance(Thing *thing, const QString &model,
                           std::function<void(uint issuedAt, uint availableAt)> continuation);

    // Composite model names (*_seamless, best_match) have no meta.json - they are blends
    // resolved per location, not grid domains. Maps to the real domain whose run identity
    // applies, or returns empty when there is no honest answer.
    static QString provenanceDomain(const QString &model);

    // Conditional GET: meta.json carries an ETag whose first field is the publication
    // timestamp, so 304 means "same run" and costs an empty body.
    QHash<QString, QByteArray> m_metaEtags;
    QHash<QString, QPair<uint, uint> > m_metaTimes;   // model -> (initialisation, availability)

    // PV plane (child): forecast GTI + integrals, satellite GTI, ensemble spread.
    void updatePlaneForecast(Thing *thing, const QString &model, bool allowFallback, Done done);
    void updatePlaneSatellite(Thing *thing);
    void updatePlaneEnsemble(Thing *thing);
    // A spread state that is no longer current carries NaN, written null in JSON.
    void invalidateSpread(Thing *thing, const StateTypeId &stateTypeId);

    // WMO weather_code -> weatherCondition enum value (with -day/-night via isDay)
    QString weatherConditionFromCode(int code, bool isDay) const;
    QString weatherDescriptionFromCode(int code) const;

    // The site of a thing: the site itself, or a plane's parent. Null when there is none.
    Thing *siteOf(Thing *thing) const;
    // The site coordinates for a thing. False when there is no site, or when its
    // coordinates are absent, the unset sentinel or out of range (SiteCoordinates).
    bool resolveCoordinates(Thing *thing, double &latitude, double &longitude) const;

    // Shared query parameters (coordinates, timezone, unixtime, optional commercial key).
    void addCommonQuery(QUrlQuery &query, double lat, double lon) const;
    // The site's declared elevation, on the site's and on each plane's forecast request.
    void addElevation(QUrlQuery &query, Thing *site) const;

    // Open-Meteo azimuth convention: 0 = South, -90 = East, +90 = West.
    // Stored azimuth is geographic (180 = South). Verified empirically, see
    // docs/OPENMETEO_API_PROBE.md sec. 1.2.
    static double azimuthToOpenMeteo(double azimuthGeographic);

    QString configuredModel() const;
    // CONTRACT_A sec. 3.2: the model that ANSWERED, not the one we asked for.
    static const char *FallbackModel;
    // True when a forecast reply carries no usable radiation at all - the signature of a
    // model that does not serve it (meteofrance_arome_france_hd serves none) or of a site
    // outside the model domain. Nulls, not an HTTP error: the API answers 200 either way.
    static bool radiationAllNull(const QVariantMap &current, const QVariantMap &hourly,
                                 const QStringList &keys);
};

#endif // INTEGRATIONPLUGINOPENMETEO_H
