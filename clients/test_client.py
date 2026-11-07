#!/usr/bin/env python3
"""Print aircraft streamed by the EuroScope WebSocket plugin.

    pip install websockets
    python test_client.py [ws://127.0.0.1:8765] [--raw] [--once]
"""
import asyncio
import json
import sys

import websockets


async def main(url: str, raw: bool, once: bool) -> None:
    async with websockets.connect(url, max_size=None) as ws:
        print(f"connected to {url}")
        async for message in ws:
            if raw:
                print(message)
            else:
                data = json.loads(message)
                print(f"\n{data['source'] or '-'}  {data['count']} aircraft")
                print(f"{'CALLSIGN':<9}{'LAT':>10}{'LON':>11}{'ALT':>7}{'GS':>5}{'HDG':>5}{'SQK':>6}  ADEP-ADES  TYPE")
                for a in data["aircraft"]:
                    fp = a["flightPlan"] or {}
                    print(f"{a['callsign']:<9}{a['lat']:>10.4f}{a['lon']:>11.4f}{a['altitude']:>7}"
                          f"{a['groundSpeed']:>5}{a['heading']:>5}{a['squawk']:>6}  "
                          f"{fp.get('origin', ''):>4}-{fp.get('destination', ''):<4}  {fp.get('aircraftType', '')}")
            if once:
                return


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    try:
        asyncio.run(main(args[0] if args else "ws://127.0.0.1:8765", "--raw" in sys.argv, "--once" in sys.argv))
    except KeyboardInterrupt:
        pass
