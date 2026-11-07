# EuroScope WebSocket Data plugin

A EuroScope 3.2.x plugin that streams live controlling data as JSON over a local WebSocket
(`ws://127.0.0.1:8765` by default). Any app, script or web page can connect and see what your
EuroScope sees: every radar target with its flight plan and controller data, the controllers online,
METARs and active runways, planned routes, and events such as handoffs as they happen. If you
allow it, clients can also send commands back, such as assume, handoff or set cleared level.

It ships as a single DLL with no dependencies, plus a browser map viewer and a fake feed for
testing clients without EuroScope.

## Ideas for what to build with it

- **A second-screen map** or traffic overview in a browser, on another monitor, tablet or PC.
- **Electronic flight strips** or a departure/arrival list that follows assumed, handoffs and cleared levels.
- **Stream overlays** for Twitch or YouTube showing your traffic, METAR and runways.
- **Stand, gate or slot tools** fed from flight plans and ground states.
- **Statistics and logging**: movements per hour, handoff times, session reports.
- **Stream Deck or hardware panels** that send commands such as assume or handoff.
- **Alerts and automation**: notify when an aircraft enters your sector, squawks an emergency code or a METAR changes.

## Quick start

1. Download the latest zip from [Releases](https://github.com/Dunkstormen/euroscope-ws-plugin/releases), or [build it yourself](docs/development.md#build-windows).
2. In EuroScope: Other SET -> Plug-ins... -> Load -> `WsDataPlugin.dll`.
3. Open `clients/viewer.html` in a browser and press Connect.

## Documentation

- [Installation and settings](docs/installation.md)
- [Protocol](docs/protocol.md)
- [Map viewer and clients](docs/viewer.md)
- [Development](docs/development.md): building, testing, CI and releases
