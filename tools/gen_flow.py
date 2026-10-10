"""Generate a random order flow and the reference answer from a separate, deliberately simple
Python exchange (dicts and lists, no shared code with the C++ engine).

Writes: flow.csv (input), ref_message.csv and ref_orderbook.csv (LOBSTER format).
Usage: gen_flow.py <out_dir> [n_orders] [levels] [seed]
"""
import random
import sys
from collections import OrderedDict

out, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 200_000
levels, seed = int(sys.argv[3]) if len(sys.argv) > 3 else 10, int(sys.argv[4]) if len(sys.argv) > 4 else 1
rnd = random.Random(seed)
warmup = 5_000

book = {"B": {}, "S": {}}          # side -> price -> OrderedDict(id -> qty)
where = {}                         # id -> (side, price)
live = False
seq = 0
flow, fm, fb = open(f"{out}/flow.csv", "w"), open(f"{out}/ref_message.csv", "w"), open(f"{out}/ref_orderbook.csv", "w")


def publish(kind, oid, size, price, side):
    global seq
    if not live:
        return
    fm.write(f"{seq},{kind},{oid},{size},{price},{1 if side == 'B' else -1}\n")
    seq += 1
    asks = sorted(book["S"])[:levels]
    bids = sorted(book["B"], reverse=True)[:levels]
    row = []
    for l in range(levels):
        row += [asks[l], sum(book["S"][asks[l]].values())] if l < len(asks) else [9999999999, 0]
        row += [bids[l], sum(book["B"][bids[l]].values())] if l < len(bids) else [-9999999999, 0]
    fb.write(",".join(map(str, row)) + "\n")


def take(oid, qty):
    side, price = where[oid]
    q = book[side][price]
    q[oid] -= qty
    if q[oid] == 0:
        del q[oid], where[oid]
        if not q:
            del book[side][price]


def new_order(oid, side, price, qty):
    flow.write(f"A,{oid},{side},{price},{qty}\n")
    other = "S" if side == "B" else "B"
    while qty and book[other]:
        best = min(book[other]) if other == "S" else max(book[other])
        if (side == "B" and best > price) or (side == "S" and best < price):
            break
        maker, mqty = next(iter(book[other][best].items()))
        fill = min(qty, mqty)
        take(maker, fill)
        qty -= fill
        publish(4, maker, fill, best, other)
    if qty:
        book[side].setdefault(price, OrderedDict())[oid] = qty
        where[oid] = (side, price)
        publish(1, oid, qty, price, side)


mid, ids = 100_000, []
for i in range(1, n + 1):
    if i == warmup:
        flow.write("S\n")
        live = True
    mid += rnd.choice((-1, 0, 0, 1))
    u = rnd.random()
    if u < 0.55 or len(where) < 50:
        side = rnd.choice("BS")
        # mostly passive, sometimes through the spread (trades), sometimes deep in the book
        off = rnd.choice((1, 1, 2, 2, 3, 5, 8, 15, 30, 60)) * (1 if rnd.random() < 0.8 else -1)
        price = mid - off if side == "B" else mid + off
        new_order(i, side, price, rnd.choice((1, 5, 10, 10, 50, 100, 100, 250, 1000)))
        ids.append(i)
    else:
        oid = ids[rnd.randrange(len(ids))]
        while oid not in where:                      # lazily drop dead ids
            ids[ids.index(oid)] = ids[-1]; ids.pop()
            oid = ids[rnd.randrange(len(ids))]
        side, price = where[oid]
        left = book[side][price][oid]
        if u < 0.9 or left == 1:
            flow.write(f"C,{oid}\n")
            take(oid, left)
            publish(3, oid, left, price, side)
        else:
            cut = rnd.randrange(1, left)
            flow.write(f"R,{oid},{cut}\n")
            take(oid, cut)
            publish(2, oid, cut, price, side)
print(f"orders {n}, published messages {seq}, resting at end {len(where)}")
