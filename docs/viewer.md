# Map viewer and clients

`clients/viewer.html` connects automatically and shows every aircraft as a plane icon rotated to its
heading, with callsign, level and speed labels and a short trail. Tracked aircraft are green. Click a
plane or a table row to select it (popup with flight plan details, "Follow selected" keeps it centred);
the selected aircraft's planned route is drawn as a dashed line. Below the table are the online
controllers, airports with METAR and active runways, and a log of events.

"Smooth motion" (on by default): EuroScope only gets a new position every few seconds, so between
reports the viewer moves each plane along its track at its ground speed. Track comes from the last two
reported positions (true north, includes wind), falling back to the reported heading. When a new report
arrives, any difference is blended out over 1.5 s instead of jumping; differences over 3 nm snap, and
extrapolation stops after 15 s without a new position. Untick it to see the raw reported positions.

## Basemap

VATSIM Radar uses the same Protomaps basemap style, but serves the tiles from its own
server (`tiles.vatsim-radar.com`), which is why it needs no API key. That server is theirs, so this
viewer does the same thing locally instead: a `.pmtiles` file on your PC, no key, works offline.

1. Download the `pmtiles` CLI for Windows from https://github.com/protomaps/go-pmtiles/releases
2. Pick a recent daily build at https://maps.protomaps.com/builds/ and cut out your area, e.g. Europe:
   ```
   pmtiles extract https://build.protomaps.com/20261101.pmtiles clients/map.pmtiles --bbox=-25,34,45,72 --maxzoom=10 --dry-run
   ```
   `--dry-run` prints the size first; drop it to download. A smaller box or lower `--maxzoom` gives a smaller
   file (zoom 10 is plenty for radar-style use; raise it for airport-level detail).
3. Run `python clients/serve.py` and open http://localhost:8000/viewer.html. The viewer picks up
   `map.pmtiles` automatically. (`serve.py` is needed because PMTiles uses HTTP range requests, which
   neither `file://` nor Python's plain `http.server` support.)

Without a local file the viewer falls back to Protomaps' public demo file, which is fine for a quick try.
Under "Map settings" you can instead point at any PMTiles URL or enter a Protomaps API key.

URL parameters `?ws=ws://host:port&key=...&pmtiles=...` override the saved settings.
The icon uses the reported (magnetic) heading, so it can be a few degrees off true north on the map.

## Terminal client

`clients/test_client.py` prints what the plugin (or the [fake feed](development.md#test-without-euroscope)) sends. It needs `pip install websockets`.
```
python clients/test_client.py
python clients/test_client.py --topics=routes --raw
python clients/test_client.py --send='{"cmd":"assume","callsign":"SAS100"}'
```
