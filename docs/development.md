# Development

## How it works

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
| `docs/` | This documentation |
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

## CI

GitHub Actions (`.github/workflows/`) run on every push to `main` and every pull request:
- **Linux**: builds `fake_feed` and runs `scripts/smoke_test.py` (starts the feed, connects over
  WebSocket, checks one snapshot).
- **Windows**: builds the 32-bit plugin DLL (`-A Win32`, checked to really be x86) plus `fake_feed.exe`,
  runs the same smoke test, and uploads a `WsDataPlugin-win32` build artifact.

### EuroScope SDK secret

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

## Releases

Push a version tag. The tag becomes the version EuroScope shows (`v0.2.0` -> `0.2.0`);
tags with a dash such as `v0.2.0-rc1` are marked as pre-releases.
```
git tag v0.2.0
git push origin v0.2.0
```
The Release workflow rebuilds everything, fails if the SDK secret is missing, and publishes a GitHub
Release with generated notes and `WsDataPlugin-<tag>-win32.zip` (DLL, `fake_feed.exe`, clients, README and docs),
a Linux `fake_feed` binary and `SHA256SUMS.txt`.

## Status and known limits

- The server, JSON and clients were tested end to end on Linux with the fake feed (20 concurrent
  clients, ping/pong, 600 aircraft / 260 KB messages, the browser viewer).
- CI builds `Plugin.cpp` against the real SDK. The protocol 2 data (controllers, airports, events,
  routes, commands) has been tested with the fake feed but not yet inside a live EuroScope session.
- No TLS (`ws://` only), and the command token is sent in clear text. Keep the default `127.0.0.1`
  bind unless you trust the network.
- Up to ~60 clients (Winsock `select()` limit), far more than needed for this.
