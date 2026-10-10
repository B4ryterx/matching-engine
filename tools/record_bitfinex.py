"""Record Bitfinex's public order-by-order book feed (every individual order, with its id),
including the checksums the exchange publishes of its own book.

Writes one flat file:
  S                               the exchange's opening snapshot follows
  O,id,price,amount,text,seq      order added or changed (price/amount in 1e-8 units; amount < 0 = sell;
                                  text = the amount exactly as sent, needed for the checksum)
  D,id,seq                        order removed
  C,checksum,seq                  exchange's CRC32 of its top 25 bids and asks at this point
  H,seq                           heartbeat (keeps the sequence unbroken)
Usage: record_bitfinex.py <out_file> [seconds] [symbol]
"""
import json
import sys
import time
from decimal import Decimal

import websocket  # websocket-client

out = sys.argv[1]
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 300
symbol = sys.argv[3] if len(sys.argv) > 3 else "tBTCUSD"
CHECKSUM, SEQUENCE = 131072, 65536

units = lambda s: int(Decimal(s) * 10**8)
ws = websocket.create_connection("wss://api-pub.bitfinex.com/ws/2", timeout=20)
ws.send(json.dumps({"event": "conf", "flags": CHECKSUM + SEQUENCE}))
ws.send(json.dumps({"event": "subscribe", "channel": "book", "symbol": symbol, "prec": "R0", "len": "250"}))
n = {"O": 0, "D": 0, "C": 0}
end = time.time() + seconds
with open(out, "w") as f:
    while time.time() < end:
        m = json.loads(ws.recv(), parse_float=str, parse_int=str)   # keep numbers as the exchange wrote them
        if isinstance(m, dict):
            continue
        if m[1] == "hb":
            f.write(f"H,{m[2]}\n")
            continue
        if m[1] == "cs":
            f.write(f"C,{int(m[2]) & 0xffffffff},{m[3]}\n"); n["C"] += 1
            continue
        if isinstance(m[1][0], list):
            f.write("S\n"); rows, seq = m[1], m[2]
        else:
            rows, seq = [m[1]], m[2]
        for oid, price, amount in rows:
            if Decimal(price) == 0:
                f.write(f"D,{oid},{seq}\n"); n["D"] += 1
            else:
                f.write(f"O,{oid},{units(price)},{units(amount)},{amount},{seq}\n"); n["O"] += 1
ws.close()
print(f"{symbol}: {n['O']} adds/changes, {n['D']} removals, {n['C']} exchange checksums over {seconds}s")
