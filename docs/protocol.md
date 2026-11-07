# Protocol

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

## hello
```json
{"type":"hello","time":1793660596000,"protocol":2,"plugin":"WebSocket Data","version":"0.2.0","source":"EKCH_APP",
 "topics":["aircraft","controllers","airports","events","routes"],"defaultTopics":["aircraft","controllers","airports","events"],
 "commands":false,"tokenRequired":false}
```

## aircraft
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

## controllers
```json
{"type":"controllers","time":1793660596000,
 "me":{"callsign":"EKCH_APP","connectionType":"direct","asel":"SAS107","controller":{...}},
 "count":1,"controllers":[{"callsign":"EKCH_TWR","name":"...","frequency":118.105,"facility":"TWR","rating":"S2",
   "positionId":"CT","sectorFile":"EKCH_TWR","lat":55.618,"lon":12.656,"range":50,"isController":true,"isMe":false}]}
```
`connectionType` is `offline`, `direct`, `proxy`, `simulatorServer`, `playback`, `simulatorClient` or `sweatbox`.
`asel` is the aircraft currently selected in EuroScope. `frequency` 199.998 means none.

## airports
```json
{"type":"airports","time":1793660596000,"count":1,"airports":[{"icao":"EKCH",
  "metar":"EKCH 071820Z 24012KT 9999 FEW030 08/04 Q1012 NOSIG","departureRunways":["22R"],"arrivalRunways":["22L"]}]}
```
METARs are the ones EuroScope downloads (the stations in your weather list). Active runways come from
the sector file and are re-checked every 30 s and whenever you change them. EuroScope gives plugins no
access to ATIS text or frequency chat, so those aren't available.

## events
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

## routes (opt-in)
```json
{"type":"routes","time":1793660596000,"count":1,"routes":[{"callsign":"SAS107",
  "points":[{"name":"SIMEG","airway":"","lat":55.9,"lon":13.4,"minutes":4,"altitude":12000,"passed":false}],
  "predictions":[{"lat":56.3,"lon":13.3,"altitude":9000}]}]}
```
`minutes` is the time to the point from now. `predictions` has one position per minute ahead. Subscribe
to `routes` and, ideally, filter it to the aircraft you care about (see [Sending messages](#sending-messages)): routes for everyone are large.

## Sending messages
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
