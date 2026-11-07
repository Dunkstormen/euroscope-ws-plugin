# Installation and settings

## Get the plugin

Download `WsDataPlugin-<version>-win32.zip` from the [latest release](https://github.com/Dunkstormen/euroscope-ws-plugin/releases)
and unzip it somewhere permanent. It holds `WsDataPlugin.dll`, `fake_feed.exe`, the clients and these docs.
To build it yourself instead, see [Development](development.md#build-windows).

## Load it in EuroScope

Other SET -> Plug-ins... -> Load -> pick `WsDataPlugin.dll`. The message window shows
`Streaming on ws://127.0.0.1:8765`. Then open `clients/viewer.html` in a browser and press Connect
(see [Map viewer and clients](viewer.md)).

## Commands

Type these in the EuroScope command line:

| Command | Effect |
|---|---|
| `.wsdata` | Status and help |
| `.wsdata port 9000` | Change port (saved in your settings, restarts server) |
| `.wsdata bind 0.0.0.0` | Listen on all interfaces so other PCs can connect (default is this PC only) |
| `.wsdata interval 2` | Push every 2 seconds |
| `.wsdata commands on` / `off` | Allow clients to send commands (off by default, saved) |
| `.wsdata token <secret>` / `off` | Require this token for commands (saved) |
| `.wsdata restart` | Restart the server |

## Security

There is no TLS (`ws://` only), and the command token is sent in clear text. Keep the default
`127.0.0.1` bind unless you trust the network. If you bind to `0.0.0.0` and enable commands, set a
token: anyone who can reach the port could otherwise control your traffic.
