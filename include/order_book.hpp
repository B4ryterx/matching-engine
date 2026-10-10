// Limit order book with price-time priority.
//
// Two ways to drive it:
//   submit()                 - act as the exchange: match the incoming order, rest the remainder.
//   add()/reduce()/cancel()  - act as a feed handler: apply changes the exchange already decided.
//
// Prices and quantities are integers (ticks and lots); no floating point in the book.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace ob {

using Price = int64_t;
using Qty = int64_t;
using OrderId = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };
inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct Trade {
    OrderId maker;  // resting order; 0 if the liquidity was seeded from a snapshot
    Side maker_side;
    Price price;
    Qty qty;
};

struct LevelView {
    Price price;
    Qty qty;
};

class OrderBook {
public:
    // Rest an order without matching. Returns false if the id is already live.
    bool add(OrderId id, Side side, Price price, Qty qty) {
        if (qty <= 0 || index_.count(id)) return false;
        uint32_t slot = alloc();
        Level& lvl = book_[idx(side)][key(side, price)];
        orders_[slot] = Order{id, price, qty, lvl.tail, NIL, side};
        if (lvl.tail != NIL) orders_[lvl.tail].next = slot; else lvl.head = slot;
        lvl.tail = slot;
        lvl.total += qty;
        index_.emplace(id, slot);
        return true;
    }

    // Match an incoming order against the opposite side: best price first, earliest order first.
    // Calls on_trade(const Trade&) after the book has been updated for each fill.
    // Returns the unfilled quantity.
    template <class F>
    Qty match(Side side, Price limit, Qty qty, F&& on_trade) {
        Side rest = opposite(side);
        Levels& levels = book_[idx(rest)];
        while (qty > 0 && !levels.empty()) {
            auto it = levels.begin();
            Price price = unkey(rest, it->first);
            if (side == Side::Buy ? price > limit : price < limit) break;
            Level& lvl = it->second;
            OrderId maker = 0;
            Qty fill;
            if (lvl.anon > 0) {  // seeded liquidity is the oldest at its level
                fill = qty < lvl.anon ? qty : lvl.anon;
                lvl.anon -= fill;
                lvl.total -= fill;
            } else {
                Order& o = orders_[lvl.head];
                maker = o.id;
                fill = qty < o.qty ? qty : o.qty;
                o.qty -= fill;
                lvl.total -= fill;
                if (o.qty == 0) unlink(lvl, lvl.head);
            }
            if (lvl.total == 0) levels.erase(it);
            qty -= fill;
            on_trade(Trade{maker, rest, price, fill});
        }
        return qty;
    }

    // Exchange behaviour: match, then rest whatever is left at the limit price.
    template <class F>
    Qty submit(OrderId id, Side side, Price price, Qty qty, F&& on_trade) {
        Qty left = match(side, price, qty, on_trade);
        if (left > 0) add(id, side, price, left);
        return left;
    }

    // Take qty off a live order (partial cancel or execution). Removes it at zero.
    bool reduce(OrderId id, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        uint32_t slot = it->second;
        Order& o = orders_[slot];
        if (qty <= 0 || qty > o.qty) return false;
        Levels& levels = book_[idx(o.side)];
        auto lit = levels.find(key(o.side, o.price));
        Level& lvl = lit->second;
        o.qty -= qty;
        lvl.total -= qty;
        if (o.qty == 0) unlink(lvl, slot);
        if (lvl.total == 0) levels.erase(lit);
        return true;
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        return it != index_.end() && reduce(id, orders_[it->second].qty);
    }

    // Liquidity known only as a level total (from a snapshot), with no order ids.
    void seed(Side side, Price price, Qty qty) {
        Level& lvl = book_[idx(side)][key(side, price)];
        lvl.anon += qty;
        lvl.total += qty;
    }

    bool reduce_anon(Side side, Price price, Qty qty) {
        Levels& levels = book_[idx(side)];
        auto it = levels.find(key(side, price));
        if (it == levels.end() || it->second.anon < qty) return false;
        it->second.anon -= qty;
        it->second.total -= qty;
        if (it->second.total == 0) levels.erase(it);
        return true;
    }

    // Feeds that publish level totals instead of orders (e.g. Binance): set a level outright.
    void set_level(Side side, Price price, Qty qty) {
        Levels& levels = book_[idx(side)];
        if (qty == 0) { levels.erase(key(side, price)); return; }
        Level& lvl = levels[key(side, price)];
        lvl.total = qty;
        lvl.anon = qty;
    }

    // Visit levels best first; stop when f(price, qty) returns false.
    template <class F>
    void walk(Side side, F&& f) const {
        for (const auto& kv : book_[idx(side)])
            if (!f(unkey(side, kv.first), kv.second.total)) break;
    }

    // Visit individual orders: best level first, earliest arrival first within a level.
    template <class F>
    void walk_orders(Side side, F&& f) const {
        for (const auto& kv : book_[idx(side)])
            for (uint32_t s = kv.second.head; s != NIL; s = orders_[s].next)
                if (!f(orders_[s].id, orders_[s].price, orders_[s].qty)) return;
    }

    Qty order_qty(OrderId id) const { return orders_[index_.find(id)->second].qty; }
    Price order_price(OrderId id) const { return orders_[index_.find(id)->second].price; }
    bool has(OrderId id) const { return index_.count(id) != 0; }
    bool empty(Side side) const { return book_[idx(side)].empty(); }
    Price best(Side side) const { return unkey(side, book_[idx(side)].begin()->first); }
    size_t live_orders() const { return index_.size(); }

    // Would a new order at this price trade immediately?
    bool crosses(Side side, Price price) const {
        Side rest = opposite(side);
        if (empty(rest)) return false;
        return side == Side::Buy ? price >= best(rest) : price <= best(rest);
    }

    // True if the order is first in the queue at its price.
    bool is_front(OrderId id) const {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Order& o = orders_[it->second];
        const Level& lvl = book_[idx(o.side)].find(key(o.side, o.price))->second;
        return lvl.anon == 0 && lvl.head == it->second;
    }

    Qty level_qty(Side side, Price price) const {
        auto it = book_[idx(side)].find(key(side, price));
        return it == book_[idx(side)].end() ? 0 : it->second.total;
    }

    // Best n levels of one side, best first. Returns how many were written.
    size_t depth(Side side, size_t n, LevelView* out) const {
        size_t i = 0;
        for (auto it = book_[idx(side)].begin(); it != book_[idx(side)].end() && i < n; ++it, ++i)
            out[i] = LevelView{unkey(side, it->first), it->second.total};
        return i;
    }

private:
    static constexpr uint32_t NIL = UINT32_MAX;

    struct Order {
        OrderId id;
        Price price;
        Qty qty;
        uint32_t prev, next;  // neighbours in the level's FIFO queue
        Side side;
    };
    struct Level {
        Qty total = 0;
        Qty anon = 0;
        uint32_t head = NIL, tail = NIL;
    };
    using Levels = std::map<Price, Level>;

    static size_t idx(Side s) { return static_cast<size_t>(s); }
    // Bids are keyed by negated price so begin() is the best level on both sides.
    static Price key(Side s, Price p) { return s == Side::Buy ? -p : p; }
    static Price unkey(Side s, Price k) { return s == Side::Buy ? -k : k; }

    uint32_t alloc() {
        if (!free_.empty()) {
            uint32_t slot = free_.back();
            free_.pop_back();
            return slot;
        }
        orders_.emplace_back();
        return static_cast<uint32_t>(orders_.size() - 1);
    }

    void unlink(Level& lvl, uint32_t slot) {
        Order& o = orders_[slot];
        if (o.prev != NIL) orders_[o.prev].next = o.next; else lvl.head = o.next;
        if (o.next != NIL) orders_[o.next].prev = o.prev; else lvl.tail = o.prev;
        index_.erase(o.id);
        free_.push_back(slot);
    }

    Levels book_[2];
    std::vector<Order> orders_;
    std::vector<uint32_t> free_;
    std::unordered_map<OrderId, uint32_t> index_;
};

}  // namespace ob
