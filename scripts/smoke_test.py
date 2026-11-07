#!/usr/bin/env python3
"""CI smoke test: start fake_feed, connect over WebSocket, check one snapshot.

    pip install websockets
    python scripts/smoke_test.py path/to/fake_feed [port]
"""
import asyncio
import json
import subprocess
import sys

import websockets

AIRCRAFT_KEYS = {"callsign", "lat", "lon", "altitude", "groundSpeed", "heading", "squawk", "flightPlan"}


async def check(url: str, expected: int) -> None:
    for _ in range(50):  # server may need a moment to start listening
        try:
            ws = await websockets.connect(url, max_size=None)
            break
        except OSError:
            await asyncio.sleep(0.2)
    else:
        raise SystemExit(f"could not connect to {url}")
    async with ws:
        data = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
        assert data["type"] == "aircraft", data
        assert data["count"] == expected == len(data["aircraft"]), data
        for a in data["aircraft"]:
            missing = AIRCRAFT_KEYS - a.keys()
            assert not missing, f"{a.get('callsign')} missing {missing}"
        await ws.ping()
    print(f"ok: {data['count']} aircraft from {url}")


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
