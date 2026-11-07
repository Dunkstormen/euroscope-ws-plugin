# EuroScope WebSocket Data plugin (prototype)

A EuroScope 3.2.x plugin that runs a small WebSocket server inside EuroScope and
streams every radar target (position, altitude, speed, heading, squawk and basic
flight plan data) as JSON, once a second, to any connected client.

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
| `src/WsServer.*` | Portable WebSocket broadcast server on a background thread |
| `src/Snapshot.h` | Plain data structs + JSON encoder (no EuroScope types) |
| `tools/fake_feed.cpp` | Same server with simulated aircraft, for testing clients without EuroScope |
| `clients/viewer.html` | Browser viewer: live map (MapLibre + Protomaps) and table |
| `clients/serve.py` | Serves the viewer and a local `map.pmtiles` basemap (range requests) |
| `clients/test_client.py` | Terminal client (`pip install websockets`) |
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
| `.wsdata restart` | Restart the server |

## Message format
One message per tick, always the full picture (clients don't need to track diffs):
```json
{
  "type": "aircraft", "time": 1793660596000, "source": "EKCH_APP", "count": 1,
  "aircraft": [{
    "callsign": "SAS107", "lat": 56.274900, "lon": 13.251600,
    "altitude": 8000, "flightLevel": 8000, "groundSpeed": 270, "heading": 47,
    "verticalSpeed": 0, "squawk": "1001", "modeC": true, "ident": false,
    "flightPlan": {
      "origin": "EKCH", "destination": "ESSA", "alternate": "", "aircraftType": "B738",
      "wakeCategory": "M", "flightRules": "I", "route": "...", "requestedAltitude": 36000,
      "clearedAltitude": 10000, "assignedSquawk": "1001", "assignedHeading": 0,
      "assignedSpeed": 0, "trackingController": "EKCH_APP", "groundState": ""
    }
  }]
}
```
`flightPlan` is `null` for targets without a correlated flight plan. `time` is Unix milliseconds,
`source` is your own controller callsign. Altitudes in feet, speeds in knots, heading in degrees.
A client that connects gets the latest snapshot straight away. Text a client sends is received
by the server but not used yet (hook for future commands).

## Map viewer
`clients/viewer.html` connects automatically and shows every aircraft as a plane icon rotated to its
heading, with callsign, level and speed labels and a short trail. Tracked aircraft are green. Click a
plane or a table row to select it (popup with details, "Follow selected" keeps it centred).

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
Like EuroScope it sends every second, but positions only change every `radarSeconds` (default 5):
```
cmake -S . -B build && cmake --build build   # Linux / macOS
./build/fake_feed 8765 8
python clients/test_client.py
```

## Status / known limits
- The server, JSON and clients were tested end to end on Linux with the fake feed (20 concurrent
  clients, ping/pong, 600 aircraft / 260 KB messages, the browser viewer).
- `Plugin.cpp` was compile-checked against a stand-in header, not the real SDK, and has not been run
  inside EuroScope yet. If a getter name differs in your SDK version, the compiler will point at it;
  `GetGroundState()` is the most likely candidate, and that line can simply be removed.
- No auth or TLS (`ws://` only). Keep the default `127.0.0.1` bind unless you trust the network.
- Up to ~60 clients (Winsock `select()` limit), far more than needed for this.
