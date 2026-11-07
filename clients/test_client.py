#!/usr/bin/env python3
"""Print what the EuroScope WebSocket plugin streams.

    pip install websockets
    python test_client.py [ws://127.0.0.1:8765] [--raw] [--once] [--topics=aircraft,events]
                          [--send='{"cmd":"assume","callsign":"SAS107"}']

--topics subscribes to the given topics (default: the server's default set).
--send sends one message after connecting (e.g. a command) and prints the reply.
--once exits after the first aircraft snapshot.
"""
import asyncio
import json
import sys

import websockets


def show(data: dict) -> None:
    t = data.get("type")
    if t == "hello":
        print(f"hello: {data['plugin']} {data['version']} protocol {data['protocol']}, source {data['source'] or '-'}, "
              f"topics {','.join(data['topics'])}, commands {'on' if data['commands'] else 'off'}")
    elif t == "aircraft":
        print(f"\n{data['source'] or '-'}  {data['count']} aircraft")
        print(f"{'CALLSIGN':<9}{'LAT':>10}{'LON':>11}{'ALT':>7}{'GS':>5}{'HDG':>5}{'SQK':>6}  ADEP-ADES  TYPE  "
              f"{'SID/STAR':<9} {'CFL':>6} TRACKED")
        for a in data["aircraft"]:
            fp = a["flightPlan"] or {}
            print(f"{a['callsign']:<9}{a['lat']:>10.4f}{a['lon']:>11.4f}{a['altitude']:>7}"
                  f"{a['groundSpeed']:>5}{a['heading']:>5}{a['squawk']:>6}  "
                  f"{fp.get('origin', ''):>4}-{fp.get('destination', ''):<4}  {fp.get('aircraftType', ''):<5} "
                  f"{fp.get('sid') or fp.get('star') or '':<9} {fp.get('clearedAltitude') or '':>6} "
                  f"{fp.get('trackingController', '')}")
    elif t == "controllers":
        me = data["me"]
        others = ", ".join(f"{c['callsign']} {c['frequency']:.3f}" for c in data["controllers"])
        print(f"controllers: me {me['callsign'] or '-'} ({me['connectionType']}, ASEL {me['asel'] or '-'}); {others}")
    elif t == "airports":
        for a in data["airports"]:
            print(f"airport {a['icao']}: dep {','.join(a['departureRunways']) or '-'} "
                  f"arr {','.join(a['arrivalRunways']) or '-'}  {a['metar']}")
    elif t == "routes":
        print(f"routes: {data['count']} ({', '.join(r['callsign'] for r in data['routes'])})")
    elif t == "event":
        extra = {k: v for k, v in data.items() if k not in ("type", "event", "time", "flightPlan")}
        print(f"event {data['event']}: {json.dumps(extra)}")
    elif t == "result":
        print(f"result {data.get('cmd')}: {'ok' if data['ok'] else 'ERROR ' + data.get('error', '')}")
    else:
        print(data)


async def main(url: str, raw: bool, once: bool, topics: list, send: str) -> None:
    async with websockets.connect(url, max_size=None) as ws:
        print(f"connected to {url}")
        if topics:
            await ws.send(json.dumps({"cmd": "subscribe", "topics": topics}))
        if send:
            await ws.send(send)
        async for message in ws:
            data = json.loads(message)
            if raw:
                print(message)
            else:
                show(data)
            if once and data.get("type") == "aircraft":
                return


def option(name: str) -> str:
    for a in sys.argv[1:]:
        if a.startswith(f"--{name}="):
            return a.split("=", 1)[1]
    return ""


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    topics = [t for t in option("topics").split(",") if t]
    try:
        asyncio.run(main(args[0] if args else "ws://127.0.0.1:8765", "--raw" in sys.argv, "--once" in sys.argv,
                         topics, option("send")))
    except KeyboardInterrupt:
        pass
