# Fixtures

Replies recorded from the Open-Meteo APIs (forecast, satellite, ensemble) with the
requests the plugin sends, used offline by the tests next to this directory.

The `latitude` / `longitude` fields have been replaced by an example point,
48.50 N 7.50 E, shifted by the same offset in every file so that the per-model grid
snapping of the original replies is kept. Everything else is the reply as recorded:
times, `utc_offset_seconds`, `elevation`, sunrise and sunset, and every weather and
irradiance value. The example point lies close enough to the recording point that
sunrise and sunset stay within a few minutes of what the coordinates imply.

No test reads the coordinates: the plugin relays what the API computes for a location
and does not compute solar geometry itself, so the tests only check the replies
against themselves (instants, day bounds, `is_day` against `sunrise` / `sunset`).
