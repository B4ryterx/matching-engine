// Hand-written cases for price-time priority.
#include <cstdio>
#include <vector>

#include "order_book.hpp"

using namespace ob;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::vector<Trade> send(OrderBook& b, OrderId id, Side s, Price p, Qty q) {
    std::vector<Trade> t;
    b.submit(id, s, p, q, [&](const Trade& x) { t.push_back(x); });
    return t;
}

int main() {
    {   // best price first
        OrderBook b;
        send(b, 1, Side::Sell, 102, 10); send(b, 2, Side::Sell, 100, 10); send(b, 3, Side::Sell, 101, 10);
        auto t = send(b, 4, Side::Buy, 101, 25);
        CHECK(t.size() == 2 && t[0].maker == 2 && t[0].price == 100 && t[1].maker == 3 && t[1].price == 101);
        CHECK(b.best(Side::Buy) == 101 && b.level_qty(Side::Buy, 101) == 5);   // remainder rests
        CHECK(b.best(Side::Sell) == 102);
    }
    {   // earliest order first at the same price
        OrderBook b;
        send(b, 1, Side::Buy, 100, 5); send(b, 2, Side::Buy, 100, 5); send(b, 3, Side::Buy, 100, 5);
        auto t = send(b, 4, Side::Sell, 100, 7);
        CHECK(t.size() == 2 && t[0].maker == 1 && t[0].qty == 5 && t[1].maker == 2 && t[1].qty == 2);
        CHECK(b.is_front(2) && b.level_qty(Side::Buy, 100) == 8 && !b.has(1) && !b.has(4));
    }
    {   // trade happens at the resting order's price, not the incoming limit
        OrderBook b;
        send(b, 1, Side::Sell, 100, 5);
        auto t = send(b, 2, Side::Buy, 105, 5);
        CHECK(t.size() == 1 && t[0].price == 100 && b.empty(Side::Buy) && b.empty(Side::Sell));
    }
    {   // no trade when prices do not cross
        OrderBook b;
        send(b, 1, Side::Sell, 101, 5);
        CHECK(send(b, 2, Side::Buy, 100, 5).empty() && !b.crosses(Side::Buy, 100) && b.crosses(Side::Buy, 101));
    }
    {   // cancel removes from the middle of the queue and keeps the order of the rest
        OrderBook b;
        send(b, 1, Side::Buy, 100, 5); send(b, 2, Side::Buy, 100, 5); send(b, 3, Side::Buy, 100, 5);
        CHECK(b.cancel(2) && !b.cancel(2) && b.level_qty(Side::Buy, 100) == 10);
        auto t = send(b, 4, Side::Sell, 100, 10);
        CHECK(t.size() == 2 && t[0].maker == 1 && t[1].maker == 3 && b.empty(Side::Buy));
    }
    {   // reducing size keeps queue position; cannot reduce by more than is left
        OrderBook b;
        send(b, 1, Side::Sell, 100, 10); send(b, 2, Side::Sell, 100, 10);
        CHECK(b.reduce(1, 4) && !b.reduce(1, 7) && b.is_front(1));
        auto t = send(b, 3, Side::Buy, 100, 6);
        CHECK(t.size() == 1 && t[0].maker == 1 && !b.has(1) && b.is_front(2));
    }
    {   // seeded (snapshot) liquidity trades before later orders at the same price
        OrderBook b;
        b.seed(Side::Sell, 100, 5); b.add(1, Side::Sell, 100, 5);
        auto t = send(b, 2, Side::Buy, 100, 8);
        CHECK(t.size() == 2 && t[0].maker == 0 && t[0].qty == 5 && t[1].maker == 1 && t[1].qty == 3);
    }
    {   // duplicate ids and empty levels
        OrderBook b;
        CHECK(b.add(1, Side::Buy, 100, 5) && !b.add(1, Side::Buy, 99, 5) && !b.cancel(9));
        CHECK(b.cancel(1) && b.empty(Side::Buy) && b.live_orders() == 0);
    }
    std::printf(failures ? "%d FAILED\n" : "all matching tests passed\n", failures);
    return failures != 0;
}
