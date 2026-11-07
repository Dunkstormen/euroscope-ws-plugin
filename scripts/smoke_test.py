#!/usr/bin/env python3
"""CI smoke test: start fake_feed, connect over WebSocket, check the protocol end to end.

Checks the retained messages a client gets on connect (hello, aircraft, controllers,
airports), then subscriptions, filters, a command with its result and event, and routes.

    pip install websockets
    python scripts/smoke_test.py path/to/fake_feed [port]
"""
import asyncio
import json
import subprocess
import sys

import websockets

AIRCRAFT_KEYS = {"callsign", "lat", "lon", "altitude", "groundSpeed", "heading", "squawk", "flightPlan"}
FLIGHT_PLAN_KEYS = {"origin", "destination", "sid", "star", "departureRunway", "arrivalRunway", "remarks",
                    "scratchpad", "directTo", "annotations", "trackedByMe", "handoffTarget", "state",
                    "sectorExitMinutes", "distanceToDestination", "clearedAltitude", "assignedMach"}
CONTROLLER_KEYS = {"callsign", "name", "frequency", "facility", "rating", "positionId", "lat", "lon", "isController"}


async def recv_until(ws, pred, what, timeout=10):
    """Read messages until pred(msg) is true; return that message."""
    async def loop():
        while True:
            msg = json.loads(await ws.recv())
            if pred(msg):
                return msg
    try:
        return await asyncio.wait_for(loop(), timeout)
    except asyncio.TimeoutError:
        raise SystemExit(f"timed out waiting for {what}")


async def connect(url):
    for _ in range(50):  # server may need a moment to start listening
        try:
            return await websockets.connect(url, max_size=None)
        except OSError:
            await asyncio.sleep(0.2)
    raise SystemExit(f"could not connect to {url}")


async def check(url: str, expected: int) -> None:
    ws = await connect(url)
    async with ws:
        # 1. Retained messages on connect, hello first.
        hello = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
        assert hello["type"] == "hello", hello
        assert hello["protocol"] >= 2 and "routes" in hello["topics"], hello
        seen = {}
        while len(seen) < 3:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
            if msg["type"] != "event":
                seen.setdefault(msg["type"], msg)
        data = seen["aircraft"]
        assert data["count"] == expected == len(data["aircraft"]), data
        for a in data["aircraft"]:
            missing = AIRCRAFT_KEYS - a.keys()
            assert not missing, f"{a.get('callsign')} missing {missing}"
            if a["flightPlan"]:
                missing = FLIGHT_PLAN_KEYS - a["flightPlan"].keys()
                assert not missing, f"{a['callsign']} flight plan missing {missing}"
                assert len(a["flightPlan"]["annotations"]) == 9
        ctl = seen["controllers"]
        assert ctl["me"]["callsign"] and ctl["count"] == len(ctl["controllers"]) > 0, ctl
        for c in ctl["controllers"]:
            missing = CONTROLLER_KEYS - c.keys()
            assert not missing, f"{c.get('callsign')} missing {missing}"
        apt = seen["airports"]
        assert apt["airports"] and "metar" in apt["airports"][0], apt
        print(f"ok: hello + {data['count']} aircraft, {ctl['count']} controllers, {apt['count']} airports")

        # 2. Bad input gets an error result, not a disconnect.
        await ws.send("not json")
        r = await recv_until(ws, lambda m: m["type"] == "result", "error result")
        assert not r["ok"] and "invalid JSON" in r["error"], r

        # 3. Command -> result + event.
        target = next(a for a in data["aircraft"] if a["flightPlan"])
        await ws.send(json.dumps({"cmd": "setClearedAltitude", "callsign": target["callsign"], "value": 7000, "id": 1}))
        r = ev = None
        while r is None or ev is None:  # the event can arrive before or after the result
            m = await recv_until(ws, lambda m: m["type"] in ("result", "event"), "command result and event")
            if m["type"] == "result" and m.get("id") == 1:
                r = m
            elif m["type"] == "event" and m["event"] == "assignedDataChanged":
                ev = m
        assert r["ok"], r
        assert ev["callsign"] == target["callsign"] and ev["flightPlan"]["clearedAltitude"] == 7000, ev
        print("ok: command, result and event")

        # 4. Filter: only one callsign.
        await ws.send(json.dumps({"cmd": "filter", "callsigns": [target["callsign"]], "id": 2}))
        r = await recv_until(ws, lambda m: m["type"] == "result" and m.get("id") == 2, "filter result")
        assert r["ok"], r
        snap = await recv_until(ws, lambda m: m["type"] == "aircraft", "filtered snapshot")
        assert [a["callsign"] for a in snap["aircraft"]] == [target["callsign"]], snap
        print("ok: filter")

        # 5. Subscribe to routes only; aircraft and controllers stop.
        await ws.send(json.dumps({"cmd": "subscribe", "topics": ["routes"], "id": 3}))
        r = await recv_until(ws, lambda m: m["type"] == "result" and m.get("id") == 3, "subscribe result")
        assert r["ok"], r
        routes = await recv_until(ws, lambda m: m["type"] == "routes", "routes", timeout=15)
        assert routes["routes"] and routes["routes"][0]["points"], routes
        for _ in range(3):
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
            assert msg["type"] in ("routes", "result"), f"unsubscribed topic delivered: {msg['type']}"
        print("ok: subscribe and routes")

        await ws.send(json.dumps({"cmd": "subscribe", "topics": ["nope"], "id": 4}))
        r = await recv_until(ws, lambda m: m["type"] == "result" and m.get("id") == 4, "bad subscribe")
        assert not r["ok"], r
        await ws.ping()
    print(f"ok: protocol checks passed against {url}")


def main() -> None:
    exe = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else "8765"
    count = 5
    proc = subprocess.Popen([exe, port, str(count), "1"])
    try:
        asyncio.run(check(f"ws://127.0.0.1:{port}", count))
    finally:
        proc.kill()
        proc.wait()


if __name__ == "__main__":
    main()
