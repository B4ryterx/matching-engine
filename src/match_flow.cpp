// Run the matching engine over a raw order flow and write what an exchange would publish,
// in LOBSTER format: one message and one book snapshot per event.
//
// Flow file lines:  A,id,B|S,price,qty   new limit order (may trade immediately)
//                   C,id                 cancel
//                   R,id,qty             reduce size
//                   S                    start publishing (everything before is the opening book)
// Usage: match_flow <flow.csv> <out_message.csv> <out_orderbook.csv> <levels>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "lobster.hpp"

using namespace ob;

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: %s <flow> <msg_out> <book_out> <levels>\n", argv[0]); return 2; }
    std::string buf = lobster::slurp(argv[1]);
    FILE* fm = std::fopen(argv[2], "w");
    FILE* fb = std::fopen(argv[3], "w");
    size_t levels = std::strtoul(argv[4], nullptr, 10);
    OrderBook book;
    std::unordered_map<OrderId, std::pair<Side, Price>> info;  // for cancel/reduce messages
    std::unordered_map<OrderId, Qty> left;
    std::vector<LevelView> a(levels), b(levels);
    bool live = false;
    long seq = 0;

    auto publish = [&](int type, OrderId id, Qty size, Price price, Side side) {
        if (!live) return;
        std::fprintf(fm, "%ld,%d,%llu,%lld,%lld,%d\n", seq++, type, static_cast<unsigned long long>(id),
                     static_cast<long long>(size), static_cast<long long>(price), side == Side::Buy ? 1 : -1);
        size_t na = book.depth(Side::Sell, levels, a.data()), nb = book.depth(Side::Buy, levels, b.data());
        for (size_t l = 0; l < levels; ++l) {
            long long ap = l < na ? a[l].price : 9999999999LL, aq = l < na ? a[l].qty : 0;
            long long bp = l < nb ? b[l].price : -9999999999LL, bq = l < nb ? b[l].qty : 0;
            std::fprintf(fb, "%lld,%lld,%lld,%lld%c", ap, aq, bp, bq, l + 1 < levels ? ',' : '\n');
        }
    };

    const char* p = buf.c_str();
    while (*p) {
        char op = *p, *e;
        if (op == 'S') {
            live = true;
        } else if (op == 'A') {
            OrderId id = std::strtoull(p + 2, &e, 10);
            Side side = e[1] == 'B' ? Side::Buy : Side::Sell;
            Price price = std::strtoll(e + 3, &e, 10);
            Qty qty = std::strtoll(e + 1, &e, 10);
            Qty rest = book.submit(id, side, price, qty, [&](const Trade& t) {
                left[t.maker] -= t.qty;
                publish(4, t.maker, t.qty, t.price, t.maker_side);
            });
            if (rest > 0) {
                info[id] = {side, price};
                left[id] = rest;
                publish(1, id, rest, price, side);
            }
        } else if (op == 'C' || op == 'R') {
            OrderId id = std::strtoull(p + 2, &e, 10);
            Qty qty = op == 'C' ? left[id] : std::strtoll(e + 1, &e, 10);
            if (book.reduce(id, qty)) {
                left[id] -= qty;
                publish(op == 'C' ? 3 : 2, id, qty, info[id].second, info[id].first);
            }
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    std::fclose(fm);
    std::fclose(fb);
    std::printf("published %ld messages\n", seq);
    return 0;
}
