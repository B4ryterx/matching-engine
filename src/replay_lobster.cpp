// Rebuild an order book from a LOBSTER message file and check it against the
// exchange's own snapshot after every message. Then measure speed per message.
//
// Usage: replay_lobster <message.csv> <orderbook.csv>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_set>

#include "lobster.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats {
    size_t applied = 0, unknown_visible = 0, unknown_hidden = 0, bad_ops = 0;
    size_t crossing_adds = 0, exec_total = 0, exec_off_best = 0, exec_queue_jump = 0;
};

struct Replayer {
    const std::vector<lobster::Message>& msgs;
    const std::vector<int64_t>& snap;
    size_t cols, levels;
    OrderBook book;
    Price deepest[2];        // worst price visible in the opening snapshot, per side
    bool window_full[2];     // false = the opening snapshot showed that whole side
    Stats st;

    Replayer(const std::vector<lobster::Message>& m, const std::vector<int64_t>& s, size_t c)
        : msgs(m), snap(s), cols(c), levels(c / 4) {}

    const int64_t* row(size_t i) const { return &snap[i * cols]; }
    static size_t col(Side s) { return s == Side::Sell ? 0 : 2; }

    // The file starts mid-session, so the book that already existed is only known
    // as level totals from the first snapshot row.
    void seed() {
        book = OrderBook();
        for (Side s : {Side::Sell, Side::Buy}) {
            size_t filled = 0;
            for (size_t l = 0; l < levels; ++l) {
                int64_t price = row(0)[4 * l + col(s)], qty = row(0)[4 * l + col(s) + 1];
                if (qty <= 0) continue;
                book.seed(s, price, qty);
                deepest[idx(s)] = price;
                ++filled;
            }
            window_full[idx(s)] = filled == levels;
        }
    }

    static size_t idx(Side s) { return static_cast<size_t>(s); }
    bool beyond_window(Side s, Price p) const {
        if (!window_full[idx(s)]) return false;
        return s == Side::Buy ? p < deepest[idx(s)] : p > deepest[idx(s)];
    }

    // check = also verify price-time priority and that new orders do not cross.
    void apply(const lobster::Message& m, bool check) {
        switch (m.type) {
            case 1:
                if (check && book.crosses(m.side, m.price)) ++st.crossing_adds;
                if (!book.add(m.id, m.side, m.price, m.size)) ++st.bad_ops;
                break;
            case 4:
                if (check) {
                    ++st.exec_total;
                    if (book.empty(m.side) || book.best(m.side) != m.price) ++st.exec_off_best;
                    else if (book.has(m.id) && book.level_qty(m.side, m.price) > 0 &&
                             !book.is_front(m.id) && !has_anon_ahead(m)) ++st.exec_queue_jump;
                }
                [[fallthrough]];
            case 2:
            case 3:
                if (book.has(m.id)) {
                    if (!book.reduce(m.id, m.size)) ++st.bad_ops;
                } else if (book.reduce_anon(m.side, m.price, m.size)) {
                    ++st.unknown_visible;  // order predates the file; came out of the seeded total
                } else if (beyond_window(m.side, m.price)) {
                    ++st.unknown_hidden;   // order predates the file and sat below the visible levels
                } else {
                    ++st.bad_ops;
                }
                break;
            default:  // 5 hidden execution, 6 cross, 7 halt: no change to the visible book
                break;
        }
        ++st.applied;
    }

    // Seeded liquidity is older than every known order, so it is always ahead in the queue.
    bool has_anon_ahead(const lobster::Message& m) { return anon_at_.count(key(m.side, m.price)) != 0; }
    static int64_t key(Side s, Price p) { return s == Side::Buy ? -p : p; }
    std::unordered_set<int64_t> anon_at_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <message.csv> <orderbook.csv>\n", argv[0]); return 2; }
    auto msgs = lobster::read_messages(argv[1]);
    size_t cols;
    auto snap = lobster::read_orderbook(argv[2], cols);
    size_t rows = snap.size() / cols;
    if (rows != msgs.size() || cols % 4) {
        std::fprintf(stderr, "file mismatch: %zu messages, %zu snapshot rows, %zu columns\n", msgs.size(), rows, cols);
        return 1;
    }
    std::printf("messages %zu | levels per side %zu\n", msgs.size(), cols / 4);

    // ---- Pass 1: correctness. Row i of the orderbook file is the book after message i.
    Replayer r(msgs, snap, cols);
    r.seed();
    for (Side s : {Side::Sell, Side::Buy})
        for (size_t l = 0; l < r.levels; ++l)
            if (r.row(0)[4 * l + Replayer::col(s) + 1] > 0)
                r.anon_at_.insert(Replayer::key(s, r.row(0)[4 * l + Replayer::col(s)]));

    size_t mismatched_rows = 0, learned = 0, compared_levels = 0;
    std::unordered_set<int64_t> learned_at;
    std::vector<LevelView> mine(r.levels);
    for (size_t i = 1; i < msgs.size(); ++i) {
        r.apply(msgs[i], true);
        const int64_t* want = r.row(i);
        bool ok = true;
        for (Side s : {Side::Sell, Side::Buy}) {
            // A level below the opening window can hold orders that predate the file.
            // The first time such a price is shown, take the unseen part from the snapshot, once.
            for (size_t l = 0; l < r.levels; ++l) {
                int64_t price = want[4 * l + Replayer::col(s)], qty = want[4 * l + Replayer::col(s) + 1];
                if (qty <= 0 || !r.beyond_window(s, price)) continue;
                Qty have = r.book.level_qty(s, price);
                if (learned_at.insert(Replayer::key(s, price)).second && qty > have) {
                    r.book.seed(s, price, qty - have);
                    r.anon_at_.insert(Replayer::key(s, price));
                    ++learned;
                }
            }
            size_t n = r.book.depth(s, r.levels, mine.data());
            for (size_t l = 0; l < r.levels; ++l) {
                int64_t price = want[4 * l + Replayer::col(s)], qty = want[4 * l + Replayer::col(s) + 1];
                if (qty <= 0) { ok &= (l >= n); continue; }   // snapshot says this level is empty
                ++compared_levels;
                ok &= (l < n && mine[l].price == price && mine[l].qty == qty);
            }
        }
        if (!ok && ++mismatched_rows <= 3)
            std::printf("  mismatch after message %zu (type %d, id %llu)\n", i, msgs[i].type,
                        static_cast<unsigned long long>(msgs[i].id));
    }
    const Stats& st = r.st;
    std::printf("\nCORRECTNESS\n");
    std::printf("snapshots checked        %zu (%zu price levels)\n", msgs.size() - 1, compared_levels);
    std::printf("mismatched snapshots     %zu\n", mismatched_rows);
    std::printf("invalid operations       %zu\n", st.bad_ops);
    std::printf("orders older than file   %zu visible, %zu below the visible levels\n", st.unknown_visible, st.unknown_hidden);
    std::printf("levels taken from a snapshot on first sight  %zu\n", learned);
    std::printf("\nPRICE-TIME PRIORITY\n");
    std::printf("new orders that would have crossed the book  %zu\n", st.crossing_adds);
    std::printf("executions %zu | away from best price %zu | ahead of an earlier order %zu\n",
                st.exec_total, st.exec_off_best, st.exec_queue_jump);

    // ---- Pass 2: speed, without the checks.
    double best_ns = 1e18;
    for (int run = 0; run < 5; ++run) {
        Replayer b(msgs, snap, cols);
        b.seed();
        auto t0 = Clock::now();
        for (size_t i = 1; i < msgs.size(); ++i) b.apply(msgs[i], false);
        double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / (msgs.size() - 1);
        best_ns = std::min(best_ns, ns);
    }
    Replayer b(msgs, snap, cols);
    b.seed();
    std::vector<uint32_t> lat(msgs.size() - 1);
    for (size_t i = 1; i < msgs.size(); ++i) {
        auto t0 = Clock::now();
        b.apply(msgs[i], false);
        lat[i - 1] = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[static_cast<size_t>(p * (lat.size() - 1))]; };
    std::printf("\nSPEED\n");
    std::printf("mean %.0f ns per message (%.1f million messages per second), best of 5 runs\n", best_ns, 1e3 / best_ns);
    std::printf("per-message, timer included: median %u ns | 99%% %u ns | 99.9%% %u ns | max %u ns\n",
                pct(0.5), pct(0.99), pct(0.999), lat.back());
    return mismatched_rows || st.bad_ops ? 1 : 0;
}
