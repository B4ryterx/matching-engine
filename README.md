# Exchange matching engine and market-data replayer

A limit order book in C++17 that matches orders by price-time priority, and replayers that
rebuild an exchange's book from its recorded feed and check it against the exchange's own snapshots.

## Results

| Check | Data | Result |
|---|---|---|
| Order-by-order rebuild vs exchange checksums | Bitfinex BTCUSD, live feed recorded for 6 minutes: 9,156 order messages (3,199 new orders, 3,257 changes, 2,700 removals) | **0 mismatches** in 124 checksums the exchange publishes of its own book (id and size of the top 25 bids and asks); 0 sequence gaps |
| Rebuild vs exchange snapshots | Binance BTCUSDT, live feed recorded for 4 minutes: 2,351 events, 15,596 level updates | **0 mismatches** in 99 snapshots (194,922 price levels, top 1,000 per side); 0 gaps |
| Engine vs independent reference | 200,000 random orders through a separate Python exchange | 224,304 published messages and book snapshots **byte-identical** |
| Rebuild from order-level messages | The same 224,304 messages in LOBSTER format, 10 levels | **0 mismatches** in 224,303 snapshots (4.47 million price levels) |
| Price-time priority | 60,561 executions in that replay | 0 away from the best price, 0 ahead of an earlier order |
| Error detection | One quantity changed by 1 unit, or one deleted level kept | Flagged whenever the level is inside the compared range |

**Speed** (Apple M4 Max, single thread, `-O2`): 30-45 ns per level update on the Binance book
(about 2,000 live levels); 70-100 ns per order message on the Bitfinex book (500 live orders); 40 ns per message on the order-level replay (median 42 ns, 99.9% 167 ns
with the timer included). The order-level test book is thin (tens of resting orders), so its figure flatters.

Figures are from one recording of each feed; a fresh recording gives different counts.

**Real order-by-order data:** the LOBSTER sample files sit behind a request form, so the order-level check
on real data uses Bitfinex's public raw book feed instead, which also lists every order with its id.
`replay_lobster` has run only on generated LOBSTER-format data; it takes the real files unchanged.

## How it works

- `include/order_book.hpp`: the book. Each side is a sorted map of price levels; bids are keyed by
  negated price so the best level is first on both sides. Each level is a first-in-first-out queue of
  orders (a linked list through a pooled array), and a hash map finds any order by id for cancels.
- `submit()` is the exchange: it trades against the best price first and the earliest order at that
  price first, at the resting order's price, then rests what is left.
- `add()/reduce()/cancel()/set_level()` are the feed handler: they apply changes the exchange already made.
- `src/replay_binance.cpp`: seeds from one REST snapshot, applies the update feed in id order, and
  compares with every later snapshot whose id falls on an event boundary.
- `src/replay_bitfinex.cpp`: applies every order add, change and removal by id, and at each exchange
  checksum recomputes the CRC32 over its own top 25 bids and asks. A size cut at the same price keeps
  the order's queue place; any other change moves it to the back.
- `src/replay_lobster.cpp`: applies one message, compares with the snapshot row for that message,
  and repeats. Recorded files start mid-session, so the opening book is seeded as level totals and
  orders older than the file are taken out of those totals.
- `tools/gen_flow.py`: a deliberately naive exchange in Python that shares no code with the engine.

## Run

Needs a C++17 compiler, `make` and Python 3. Recorded market data is not in the repository; the recorders below fetch a fresh sample.

```
make test                                              # unit tests, reference comparison, replay
python3 tools/record_binance.py data/binance 240       # record a fresh feed (needs websocket-client)
build/replay_binance data/binance/events.csv data/binance/snapshots.csv
python3 tools/record_bitfinex.py data/bitfinex/feed.csv 360
build/replay_bitfinex data/bitfinex/feed.csv
```

## Limits

- Binance publishes level totals, not individual orders, so that check proves the level rebuild only.
- Bitfinex ranks orders within a price by id and an order keeps its id when it is repriced, so its
  checksum proves which orders are in the book and their sizes, not arrival order within a price.
  Arrival order (time priority) is tested on generated data and in the unit tests.
- The Bitfinex checksum covers the top 25 orders per side; errors deeper in the book are not detected.
- The Binance comparison covers the top 1,000 levels per side, which is what the snapshot endpoint returns.
- No market orders, stop orders, self-trade prevention, auctions or hidden orders.
