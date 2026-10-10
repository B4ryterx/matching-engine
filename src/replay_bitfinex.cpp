// Rebuild Bitfinex's order book order by order from its recorded feed and check it against
// the checksums the exchange publishes of its own book (CRC32 over the id and amount of the
// top 25 bids and top 25 asks). File comes from tools/record_bitfinex.py.
//
// Usage: replay_bitfinex <feed.csv>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "lobster.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Msg { char type; OrderId id; Price price; Qty amount; int64_t seq; uint32_t checksum; std::string text; };

uint32_t crc32(const std::string& s) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<Msg> parse(const std::string& path) {
    std::string buf = lobster::slurp(path);
    std::vector<Msg> out;
    const char* p = buf.c_str();
    while (*p) {
        Msg m{*p, 0, 0, 0, 0, 0, {}};
        char* e = const_cast<char*>(p) + 1;
        if (m.type == 'O') {
            m.id = std::strtoull(e + 1, &e, 10);
            m.price = std::strtoll(e + 1, &e, 10);
            m.amount = std::strtoll(e + 1, &e, 10);
            const char* t = e + 1;
            const char* c = std::strchr(t, ',');
            m.text.assign(t, c);
            m.seq = std::strtoll(c + 1, &e, 10);
        } else if (m.type == 'D') {
            m.id = std::strtoull(e + 1, &e, 10);
            m.seq = std::strtoll(e + 1, &e, 10);
        } else if (m.type == 'C') {
            m.checksum = static_cast<uint32_t>(std::strtoull(e + 1, &e, 10));
            m.seq = std::strtoll(e + 1, &e, 10);
        } else if (m.type == 'H') {
            m.seq = std::strtoll(e + 1, &e, 10);
        }
        out.push_back(std::move(m));
        while (*e && *e != '\n') ++e;
        p = *e ? e + 1 : e;
    }
    return out;
}

// A size cut at the same price keeps the order's place in the queue (a partial fill);
// anything else is a new place at the back.
void apply_order(OrderBook& book, const Msg& m, size_t* adds, size_t* changes) {
    Side side = m.amount > 0 ? Side::Buy : Side::Sell;
    Qty qty = m.amount > 0 ? m.amount : -m.amount;
    if (!book.has(m.id)) { ++*adds; book.add(m.id, side, m.price, qty); return; }
    ++*changes;
    Qty have = book.order_qty(m.id);
    if (book.order_price(m.id) == m.price && qty < have) { book.reduce(m.id, have - qty); return; }
    if (book.order_price(m.id) == m.price && qty == have) return;
    book.cancel(m.id);
    book.add(m.id, side, m.price, qty);
}

struct Entry { OrderId id; Price price; size_t arrival; };

// Top 25 orders of one side as the exchange ranks them: best price first, then lowest id.
// Returns false if any price level's arrival order differs from id order.
bool top25(const OrderBook& book, Side side, std::vector<Entry>* out) {
    out->clear();
    size_t arrival = 0, level_start = 0;
    Price current = 0;
    bool fifo_matches = true;
    auto close_level = [&]() {
        for (size_t i = level_start + 1; i < out->size(); ++i) fifo_matches &= (*out)[i - 1].id < (*out)[i].id;
        std::sort(out->begin() + level_start, out->end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    };
    book.walk_orders(side, [&](OrderId id, Price price, Qty) {
        if (!out->empty() && price != current) {
            close_level();
            if (out->size() >= 25) return false;
            level_start = out->size();
        }
        current = price;
        out->push_back(Entry{id, price, arrival++});
        return true;
    });
    close_level();
    if (out->size() > 25) out->resize(25);
    return fifo_matches;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <feed.csv>\n", argv[0]); return 2; }
    std::vector<Msg> msgs = parse(argv[1]);
    OrderBook book;
    std::unordered_map<OrderId, std::string> text;   // amount exactly as the exchange wrote it
    size_t adds = 0, changes = 0, removals = 0, unknown = 0, gaps = 0, checks = 0, mismatched = 0, crossed = 0;
    int64_t last_seq = 0;
    std::vector<Entry> bids, asks;
    std::string s;
    // The opening snapshot is not in time order; ids are, so seed each snapshot oldest order first.
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].type != 'S') continue;
        size_t j = i + 1;
        while (j < msgs.size() && msgs[j].type == 'O' && msgs[j].seq == msgs[i + 1].seq) ++j;
        std::sort(msgs.begin() + i + 1, msgs.begin() + j, [](const Msg& a, const Msg& b) { return a.id < b.id; });
    }
    for (const Msg& m : msgs) {
        if (m.type == 'S') { book = OrderBook(); text.clear(); last_seq = 0; continue; }
        if (last_seq && m.seq != last_seq && m.seq != last_seq + 1) ++gaps;
        last_seq = m.seq;
        if (m.type == 'O') {
            apply_order(book, m, &adds, &changes);
            text[m.id] = m.text;
        } else if (m.type == 'D') {
            if (book.cancel(m.id)) { ++removals; text.erase(m.id); } else ++unknown;
        } else if (m.type == 'C') {
            top25(book, Side::Buy, &bids);
            top25(book, Side::Sell, &asks);
            s.clear();
            for (size_t k = 0; k < 25; ++k) {
                if (k < bids.size()) { s += std::to_string(bids[k].id); s += ':'; s += text[bids[k].id]; s += ':'; }
                if (k < asks.size()) { s += std::to_string(asks[k].id); s += ':'; s += text[asks[k].id]; s += ':'; }
            }
            if (!s.empty()) s.pop_back();
            ++checks;
            if (crc32(s) != m.checksum) { ++mismatched; if (mismatched <= 3) std::printf("  mismatch at sequence %lld\n", static_cast<long long>(m.seq)); }
            if (!book.empty(Side::Buy) && !book.empty(Side::Sell) && book.best(Side::Buy) >= book.best(Side::Sell)) ++crossed;
        }
    }
    std::printf("messages %zu | orders added %zu, changed %zu, removed %zu | live at end %zu\n", msgs.size(), adds, changes, removals, book.live_orders());
    std::printf("\nCORRECTNESS\n");
    std::printf("gaps in the sequence numbers        %zu\n", gaps);
    std::printf("removals of unknown orders          %zu\n", unknown);
    std::printf("exchange checksums compared         %zu (each covers the top 25 bids and asks, order by order)\n", checks);
    std::printf("mismatched checksums                %zu\n", mismatched);
    std::printf("checks with bid >= ask              %zu\n", crossed);

    double best_ns = 1e18;
    size_t n_ops = adds + changes + removals + unknown;
    size_t dummy = 0;
    for (int run = 0; run < 20; ++run) {
        OrderBook b;
        auto t0 = Clock::now();
        for (const Msg& m : msgs) {
            if (m.type == 'S') b = OrderBook();
            else if (m.type == 'O') apply_order(b, m, &dummy, &dummy);
            else if (m.type == 'D') b.cancel(m.id);
        }
        best_ns = std::min(best_ns, std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
    }
    std::printf("\nSPEED\n%.0f ns per order message (best of 20 runs)\n", best_ns / n_ops);
    return mismatched || gaps ? 1 : 0;
}
