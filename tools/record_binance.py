"""Record Binance's public order-book feed for one symbol, plus the exchange's own
snapshots taken every few seconds, so the rebuilt book can be checked against them.

Writes flat files (prices and sizes as integers, 1e-8 units):
  events.csv     E,first_update_id,last_update_id   then  b|a,price,qty   per changed level
  snapshots.csv  S,last_update_id                   then  b|a,price,qty   per level
Usage: record_binance.py <out_dir> [seconds] [symbol]
"""
import json
import sys
import threading
import time
import urllib.request

import websocket  # websocket-client

out = sys.argv[1]
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 240
symbol = sys.argv[3] if len(sys.argv) > 3 else "BTCUSDT"
REST = f"https://api.binance.com/api/v3/depth?symbol={symbol}&limit=1000"
WS = f"wss://stream.binance.com:443/ws/{symbol.lower()}@depth@100ms"


def units(s):                      # "82596.93000000" -> 8259693000000, exact
    whole, frac = s.split(".")
    return int(whole + frac.ljust(8, "0")[:8])


def levels(f, tag, rows):
    for p, q in rows:
        f.write(f"{tag},{units(p)},{units(q)}\n")


n_events = 0
stop = threading.Event()


def stream():
    global n_events
    with open(f"{out}/events.csv", "w") as f:
        while not stop.is_set():
            try:                       # on a stall, reconnect; the replayer re-seeds across the gap
                ws = websocket.create_connection(WS, timeout=10)
                while not stop.is_set():
                    m = json.loads(ws.recv())
                    if "U" not in m:
                        continue
                    f.write(f"E,{m['U']},{m['u']}\n")
                    levels(f, "b", m["b"])
                    levels(f, "a", m["a"])
                    n_events += 1
                ws.close()
            except (websocket.WebSocketException, OSError) as e:
                print("stream interrupted, reconnecting:", type(e).__name__, file=sys.stderr)


t = threading.Thread(target=stream, daemon=True)
t.start()
time.sleep(3)                      # let the stream get ahead of the first snapshot
n_snaps, end = 0, time.time() + seconds
with open(f"{out}/snapshots.csv", "w") as f:
    while time.time() < end and t.is_alive():
        s = json.load(urllib.request.urlopen(REST, timeout=10))
        f.write(f"S,{s['lastUpdateId']}\n")
        levels(f, "b", s["bids"])
        levels(f, "a", s["asks"])
        n_snaps += 1
        time.sleep(2)
time.sleep(3)                      # let the stream pass the last snapshot
stop.set()
t.join(timeout=15)
print(f"{symbol}: {n_events} feed events, {n_snaps} snapshots over {seconds}s")
