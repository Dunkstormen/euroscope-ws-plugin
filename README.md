# EuroScope WebSocket Data plugin (prototype)

A EuroScope 3.2.x plugin that runs a small WebSocket server inside EuroScope and
streams JSON to any connected client: every radar target with its full flight plan and
controller data (once a second), the online controllers, METARs and active runways,
planned routes, and events as they happen. Clients pick the topics they want, can filter
aircraft, and (if you allow it) can send commands such as assume, handoff or set cleared level.

No third-party dependencies: the WebSocket server, SHA-1 and JSON encoding are
built in, and the C runtime is linked statically, so you ship a single DLL.

```
EuroScope thread  OnTimer() -> read SDK -> build JSON -> WsServer::Broadcast()
Server thread     select() loop -> handshake / ping / close -> send JSON to clients
```
The SDK is only ever called from EuroScope's own thread.

## Layout
| Path | What |
|---|---|
| `src/Plugin.cpp` | The EuroScope plugin: captures data in `OnTimer`, `.wsdata` command |
| `src/WsServer.*` | Portable WebSocket server with topics and retained messages, on a background thread |
| `src/Snapshot.h` | Plain data structs + JSON encoders (no EuroScope types) |
| `src/Protocol.h` | Client protocol shared by plugin and fake feed: subscriptions, filters, auth, commands |
| `src/JsonParse.h` | Small JSON reader for client messages |
| `tools/fake_feed.cpp` | Same server with simulated aircraft, for testing clients without EuroScope |
| `clients/viewer.html` | Browser viewer: live map (MapLibre + Protomaps) and table |
| `clients/serve.py` | Serves the viewer and a local `map.pmtiles` basemap (range requests) |
| `clients/test_client.py` | Terminal client (`pip install websockets`) |
| `scripts/smoke_test.py` | CI check: runs `fake_feed` and checks the protocol over WebSocket (topics, filters, commands) |
| `.github/workflows/` | CI (build + smoke test) and tag-triggered releases |
| `sdk/` | Put `EuroScopePlugIn.h` and `EuroScopePlugInDll.lib` here (not included) |

## Build (Windows)
1. Install Visual Studio 2022 with "Desktop development with C++" (includes CMake).
2. Copy `EuroScopePlugIn.h` and `EuroScopePlugInDll.lib` from your EuroScope install
   (usually the `PlugInEnvironment` folder) into `sdk/`.
3. From a Developer PowerShell in this folder:
   ```
   cmake -S . -B build -A Win32
   cmake --build build --config Release
   ```
   The `-A Win32` part matters: EuroScope is 32-bit, so the DLL must be too.
4. Output: `build/Release/WsDataPlugin.dll` (and `fake_feed.exe`).

## Load it in EuroScope
Other SET -> Plug-ins... -> Load -> pick `WsDataPlugin.dll`. The message window shows
`Streaming on ws://127.0.0.1:8765`. Then open `clients/viewer.html` in a browser and press Connect.

Commands (type in the EuroScope command line):
| Command | Effect |
|---|---|
| `.wsdata` | Status and help |
| `.wsdata port 9000` | Change port (saved in your settings, restarts server) |
| `.wsdata bind 0.0.0.0` | Listen on all interfaces so other PCs can connect (default is this PC only) |
| `.wsdata interval 2` | Push every 2 seconds |
| `.wsdata commands on` / `off` | Allow clients to send commands (off by default, saved) |
| `.wsdata token <secret>` / `off` | Require this token for commands (saved) |
| `.wsdata restart` | Restart the server |

## Protocol
Every message is a JSON object with a `type`. Messages belong to topics. A new client is subscribed to
`aircraft`, `controllers`, `airports` and `events`, and straight away gets `hello` plus the latest
`aircraft`, `controllers` and `airports` message, so it doesn't wait for the next tick. `time` is Unix
milliseconds everywhere. Altitudes in feet, speeds in knots, headings in degrees, distances in nm.

| Topic | Sent | Content |
|---|---|---|
| `hello` | on connect, and when settings change | plugin version, protocol version, topics, whether commands are on |
| `aircraft` | every interval (1 s) | every radar target with flight plan, controller assigned data and coordination |
| `controllers` | every interval | everyone online, plus `me` (own callsign, connection type, selected aircraft) |
| `airports` | when a METAR or active runway changes | METAR and active departure/arrival runways per airport |
| `events` | as they happen | flight plan and assigned data changes, tracking and handoffs, connects/disconnects |
| `routes` | every 5 s, **opt-in** | planned route points with ETA and profile altitude, and predicted positions |

### hello
```json
{"type":"hello","time":1793660596000,"protocol":2,"plugin":"WebSocket Data","version":"0.2.0","source":"EKCH_APP",
 "topics":["aircraft","controllers","airports","events","routes"],"defaultTopics":["aircraft","controllers","airports","events"],
 "commands":false,"tokenRequired":false}
```

### aircraft
```json
{
  "type": "aircraft", "time": 1793660596000, "source": "EKCH_APP", "count": 1,
  "aircraft": [{
    "callsign": "SAS107", "lat": 56.274900, "lon": 13.251600,
    "altitude": 8000, "flightLevel": 8000, "groundSpeed": 270, "heading": 47,
    "verticalSpeed": 0, "squawk": "1001", "modeC": true, "ident": false,
    "flightPlan": {
      "origin": "EKCH", "destination": "ESSA", "alternate": "", "aircraftType": "B738",
      "aircraftInfo": "B738/M-SDE2E3FGHIRWY/LB1", "wakeCategory": "M", "engineType": "J", "engineCount": 2,
      "capabilities": "L", "flightRules": "I", "route": "...", "remarks": "PBN/A1B1C1D1 /V/",
      "sid": "SIMEG1A", "star": "", "departureRunway": "22R", "arrivalRunway": "",
      "trueAirspeed": 450, "estimatedDeparture": "1200", "actualDeparture": "", "enrouteTime": "1:15",
      "fuelTime": "3:30", "pilotName": "...", "requestedAltitude": 36000,
      "clearedAltitude": 10000, "assignedSquawk": "1001", "assignedHeading": 0, "assignedSpeed": 0,
      "assignedMach": 0.00, "assignedRate": 0, "directTo": "", "scratchpad": "", "communicationType": "V",
      "annotations": ["", "", "", "", "", "", "", "", ""], "clearance": false, "groundState": "",
      "trackingController": "EKCH_APP", "trackedByMe": true, "handoffTarget": "", "nextController": "EKDK_CTR",
      "state": "assumed", "sectorEntryMinutes": -1, "sectorExitMinutes": 10, "entryPoint": "", "exitPoint": "SIMEG",
      "distanceToDestination": 300.0, "distanceFromOrigin": 20.0
    }
  }]
}
```
`flightPlan` is `null` for targets without a correlated flight plan. `source` is your own callsign.
Zero or empty means "not set" for the assigned values. `assignedMach` is a Mach number (0.78).
`state` is one of `notConcerned`, `notified`, `coordinated`, `transferToMe`, `transferFromMe`, `assumed`,
`redundant`. `annotations` are the 9 flight strip annotations. `sectorEntryMinutes`/`sectorExitMinutes`
are -1 when not applicable.

### controllers
```json
{"type":"controllers","time":1793660596000,
 "me":{"callsign":"EKCH_APP","connectionType":"direct","asel":"SAS107","controller":{...}},
 "count":1,"controllers":[{"callsign":"EKCH_TWR","name":"...","frequency":118.105,"facility":"TWR","rating":"S2",
   "positionId":"CT","sectorFile":"EKCH_TWR","lat":55.618,"lon":12.656,"range":50,"isController":true,"isMe":false}]}
```
`connectionType` is `offline`, `direct`, `proxy`, `simulatorServer`, `playback`, `simulatorClient` or `sweatbox`.
`asel` is the aircraft currently selected in EuroScope. `frequency` 199.998 means none.

### airports
```json
{"type":"airports","time":1793660596000,"count":1,"airports":[{"icao":"EKCH",
  "metar":"EKCH 071820Z 24012KT 9999 FEW030 08/04 Q1012 NOSIG","departureRunways":["22R"],"arrivalRunways":["22L"]}]}
```
METARs are the ones EuroScope downloads (the stations in your weather list). Active runways come from
the sector file and are re-checked every 30 s and whenever you change them. EuroScope gives plugins no
access to ATIS text or frequency chat, so those aren't available.

### events
```json
{"type":"event","event":"assignedDataChanged","time":1793660596000,"callsign":"SAS107","field":"clearedAltitude","flightPlan":{...}}
```
| `event` | Fields |
|---|---|
| `flightPlanUpdated` | `callsign`, `flightPlan` (filed data changed) |
| `assignedDataChanged` | `callsign`, `field` (`clearedAltitude`, `finalAltitude`, `squawk`, `scratchpad`, `heading`, `speed`, `mach`, `rate`, `directTo`, `communicationType`, `groundState`, `clearance`, `departureSequence`), `flightPlan` |
| `trackingChanged` | `callsign`, `from`, `to` (empty = not tracked) |
| `handoffInitiated` | `callsign`, `from`, `to` |
| `handoffEnded` | `callsign`, `to`, `accepted` |
| `stripPushed` | `callsign`, `from`, `to` |
| `aircraftDisconnected` | `callsign` |
| `controllerConnected` | `controller` |
| `controllerDisconnected` | `callsign` |
| `metarUpdated` | `icao`, `metar` |
| `runwaysChanged` | (the new runways follow in an `airports` message) |

Tracking and handoff events come from comparing ticks, so they arrive with the next push.

### routes (opt-in)
```json
{"type":"routes","time":1793660596000,"count":1,"routes":[{"callsign":"SAS107",
  "points":[{"name":"SIMEG","airway":"","lat":55.9,"lon":13.4,"minutes":4,"altitude":12000,"passed":false}],
  "predictions":[{"lat":56.3,"lon":13.3,"altitude":9000}]}]}
```
`minutes` is the time to the point from now. `predictions` has one position per minute ahead. Subscribe
to `routes` and, ideally, filter it to the aircraft you care about (see below): routes for everyone are large.

### Sending messages
Clients send JSON objects with a `cmd`. Every message gets a `{"type":"result","cmd":...,"ok":true}` or
`{"type":"result","ok":false,"error":"..."}` reply; include an `id` and it is echoed back.

| Message | Effect |
|---|---|
| `{"cmd":"subscribe","topics":["aircraft","routes"]}` | Replace your topics (`hello` is always on) |
| `{"cmd":"filter","tracked":"me"}` | Only aircraft tracked by me (`"any"`: tracked by anyone) |
| `{"cmd":"filter","area":[55,10,57,15]}` | Only aircraft inside `[minLat, minLon, maxLat, maxLon]` |
| `{"cmd":"filter","topic":"routes","callsigns":["SAS107"]}` | Filter `routes` (or `aircraft`, the default) by callsign |
| `{"cmd":"filter"}` | Remove the `aircraft` filter (add `"topic":"routes"` for routes) |
| `{"cmd":"auth","token":"secret"}` | Authenticate for commands (or add `"token"` to any message) |
| `{"cmd":"ping"}` | Replies with a result |

Filters can be combined in one message. **Commands** act on EuroScope and only work after
`.wsdata commands on` (and with the token, if one is set). They run on EuroScope's own thread within
a second, and EuroScope's usual rules apply: most changes need the aircraft to be tracked by you or nobody.

| Command | Extra fields |
|---|---|
| `assume`, `drop`, `acceptHandoff`, `refuseHandoff`, `select` (make it the ASEL) | `callsign` |
| `handoff` | `callsign`, `to` (controller callsign) |
| `setClearedAltitude`, `setFinalAltitude` | `callsign`, `value` (ft) |
| `setHeading`, `setSpeed`, `setRate` | `callsign`, `value` (0 clears) |
| `setMach` | `callsign`, `value` (0.78 or 78) |
| `setSquawk` | `callsign`, `value` ("4 octal digits") |
| `setScratchpad`, `setDirectTo` | `callsign`, `value` (text) |
| `setCommunicationType` | `callsign`, `value` (`v`, `r` or `t`) |
| `setAnnotation` | `callsign`, `index` (0-8), `value` |
| `setRoute` | `callsign`, `value` (new route, amends the flight plan) |

Example: `{"cmd":"setClearedAltitude","callsign":"SAS107","value":7000,"id":1}`. If you bind to
`0.0.0.0` and enable commands, set a token: anyone who can reach the port could otherwise control your traffic.

## Map viewer
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

Basemap: VATSIM Radar uses the same Protomaps basemap style, but serves the tiles from its own
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

## Test without EuroScope
`fake_feed [port] [aircraftCount] [radarSeconds]` serves the same JSON with moving fake aircraft, on Windows or Linux.
Like EuroScope it sends every second, but positions only change every `radarSeconds` (default 5).
It also sends fake controllers, an airport, routes and events, and accepts a subset of the commands
(always enabled, no token) so you can try them:
```
cmake -S . -B build && cmake --build build   # Linux / macOS
./build/fake_feed 8765 8
python clients/test_client.py
python clients/test_client.py --send='{"cmd":"assume","callsign":"SAS100"}'
python clients/test_client.py --topics=routes --raw
```

## CI and releases
GitHub Actions (`.github/workflows/`) run on every push to `main` and every pull request:
- **Linux**: builds `fake_feed` and runs `scripts/smoke_test.py` (starts the feed, connects over
  WebSocket, checks one snapshot).
- **Windows**: builds the 32-bit plugin DLL (`-A Win32`, checked to really be x86) plus `fake_feed.exe`,
  runs the same smoke test, and uploads a `WsDataPlugin-win32` build artifact.

The EuroScope SDK can't be committed, so CI reads it from a repository secret called `EUROSCOPE_SDK`:
base64 of an xz-compressed tar holding `EuroScopePlugIn.h` and `EuroScopePlugInDll.lib`. xz matters:
secrets max out at 48 KB, and the 3.2.x SDK is about 42 KB this way but about 60 KB with gzip.
Create the value from this folder in Git Bash (or WSL/Linux), with the SDK files in `sdk/`:
```
tar -cf - -C sdk EuroScopePlugIn.h EuroScopePlugInDll.lib | xz -9e | base64 -w0 > sdk/EUROSCOPE_SDK-secret.txt
```
Paste the file's contents into Settings -> Secrets and variables -> Actions -> New repository secret
(or run `gh secret set EUROSCOPE_SDK < sdk/EUROSCOPE_SDK-secret.txt`), then delete the file.
Without the secret, for example on pull requests from forks, the Windows job still builds and tests
`fake_feed.exe` and skips the DLL with a warning.

**Releasing**: push a version tag. The tag becomes the version EuroScope shows (`v0.2.0` -> `0.2.0`);
tags with a dash such as `v0.2.0-rc1` are marked as pre-releases.
```
git tag v0.2.0
git push origin v0.2.0
```
The Release workflow rebuilds everything, fails if the SDK secret is missing, and publishes a GitHub
Release with generated notes and `WsDataPlugin-<tag>-win32.zip` (DLL, `fake_feed.exe`, clients, README),
a Linux `fake_feed` binary and `SHA256SUMS.txt`.

## Status / known limits
- The server, JSON and clients were tested end to end on Linux with the fake feed (20 concurrent
  clients, ping/pong, 600 aircraft / 260 KB messages, the browser viewer).
- CI builds `Plugin.cpp` against the real SDK. The protocol 2 data (controllers, airports, events,
  routes, commands) has been tested with the fake feed but not yet inside a live EuroScope session.
- No TLS (`ws://` only), and the command token is sent in clear text. Keep the default `127.0.0.1`
  bind unless you trust the network.
- Up to ~60 clients (Winsock `select()` limit), far more than needed for this.
