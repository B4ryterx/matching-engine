// Rebuild Binance's order book from its recorded update feed and check it against
// the exchange's own REST snapshots. Files come from tools/record_binance.py.
//
// Binance publishes level totals, not individual orders. Each feed event carries a range of
// update ids [U, u]; a snapshot carries the id it is current to. A snapshot can be compared
// exactly only when its id equals the last id of an event.
//
// Usage: replay_binance <events.csv> <snapshots.csv>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lobster.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Update { Side side; Price price; Qty qty; };
struct Event { int64_t first, last; size_t begin, end; };
struct Snapshot { int64_t id; std::vector<LevelView> bids, asks; };

void parse(const std::string& path, std::vector<Event>* events, std::vector<Update>* updates,
           std::vector<Snapshot>* snaps) {
    std::string buf = lobster::slurp(path);
    const char* p = buf.c_str();
    while (*p) {
        char tag = *p, *e;
        int64_t x = std::strtoll(p + 2, &e, 10);
        int64_t y = (*e == ',') ? std::strtoll(e + 1, &e, 10) : 0;
        if (tag == 'E') {
            events->push_back(Event{x, y, updates->size(), updates->size()});
        } else if (tag == 'S') {
            snaps->push_back(Snapshot{x, {}, {}});
        } else if (events) {
            updates->push_back(Update{tag == 'b' ? Side::Buy : Side::Sell, x, y});
            events->back().end = updates->size();
        } else {
            (tag == 'b' ? snaps->back().bids : snaps->back().asks).push_back(LevelView{x, y});
        }
        p = (*e == '\n') ? e + 1 : e;
    }
}

void seed(OrderBook& book, const Snapshot& s) {
    book = OrderBook();
    for (const auto& l : s.bids) book.set_level(Side::Buy, l.price, l.qty);
    for (const auto& l : s.asks) book.set_level(Side::Sell, l.price, l.qty);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <events.csv> <snapshots.csv>\n", argv[0]); return 2; }
    std::vector<Event> events;
    std::vector<Update> updates;
    std::vector<Snapshot> snaps;
    parse(argv[1], &events, &updates, nullptr);
    parse(argv[2], nullptr, nullptr, &snaps);
    std::printf("feed events %zu | level updates %zu | exchange snapshots %zu\n", events.size(), updates.size(), snaps.size());

    OrderBook book;
    size_t si = 0;
    while (si < snaps.size() && snaps[si].id + 1 < events.front().first) ++si;   // first snapshot the feed covers
    if (si == snaps.size()) { std::fprintf(stderr, "no snapshot overlaps the feed\n"); return 1; }
    seed(book, snaps[si]);
    // Every bid at or above lo and every ask at or below hi is known from here on.
    Price lo = snaps[si].bids.back().price, hi = snaps[si].asks.back().price;
    int64_t seed_id = snaps[si].id, prev_last = 0;
    ++si;

    size_t gaps = 0, exact = 0, mid_event = 0, mismatched = 0, levels_checked = 0, crossed = 0;
    std::vector<size_t> per_snapshot;
    std::vector<LevelView> mine;
    for (const Event& ev : events) {
        if (ev.last <= seed_id) continue;                       // already inside the seed snapshot
        if (prev_last == 0 ? ev.first > seed_id + 1 : ev.first != prev_last + 1) ++gaps;
        prev_last = ev.last;
        for (size_t k = ev.begin; k < ev.end; ++k) book.set_level(updates[k].side, updates[k].price, updates[k].qty);
        if (!book.empty(Side::Buy) && !book.empty(Side::Sell) && book.best(Side::Buy) >= book.best(Side::Sell)) ++crossed;

        for (; si < snaps.size() && snaps[si].id <= ev.last; ++si) {
            const Snapshot& s = snaps[si];
            if (s.id != ev.last) { ++mid_event; continue; }     // book state at that id is not observable
            ++exact;
            bool ok = true;
            size_t n = 0;
            for (Side side : {Side::Buy, Side::Sell}) {
                const auto& want = side == Side::Buy ? s.bids : s.asks;
                Price bound = side == Side::Buy ? std::max(lo, want.back().price) : std::min(hi, want.back().price);
                auto in_range = [&](Price p) { return side == Side::Buy ? p >= bound : p <= bound; };
                mine.clear();
                book.walk(side, [&](Price p, Qty q) { if (!in_range(p)) return false; mine.push_back({p, q}); return true; });
                size_t w = 0;
                while (w < want.size() && in_range(want[w].price)) ++w;
                ok &= (w == mine.size());
                for (size_t l = 0; ok && l < w; ++l) ok &= (mine[l].price == want[l].price && mine[l].qty == want[l].qty);
                n += w;
            }
            levels_checked += n;
            per_snapshot.push_back(n);
            if (!ok) { ++mismatched; std::printf("  mismatch at snapshot id %lld\n", static_cast<long long>(s.id)); }
        }
    }
    std::sort(per_snapshot.begin(), per_snapshot.end());
    std::printf("\nCORRECTNESS\n");
    std::printf("gaps in the feed                    %zu\n", gaps);
    std::printf("snapshots comparable (id on an event boundary)  %zu; not comparable (id inside an event) %zu\n", exact, mid_event);
    std::printf("mismatched snapshots                %zu\n", mismatched);
    if (!per_snapshot.empty())
        std::printf("price levels checked                %zu (per snapshot: min %zu, median %zu)\n", levels_checked,
                    per_snapshot.front(), per_snapshot[per_snapshot.size() / 2]);
    std::printf("events leaving bid >= ask           %zu\n", crossed);

    double best_ns = 1e18;
    for (int run = 0; run < 20; ++run) {
        OrderBook b;
        seed(b, snaps[0]);
        auto t0 = Clock::now();
        for (const Update& u : updates) b.set_level(u.side, u.price, u.qty);
        best_ns = std::min(best_ns, std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
    }
    std::printf("\nSPEED\n%.0f ns per level update, %.0f ns per feed event (best of 20 runs)\n",
                best_ns / updates.size(), best_ns / events.size());
    return mismatched || gaps ? 1 : 0;
}
