// Reader for LOBSTER-format files (lobsterdata.com).
//   message file:   time,type,order_id,size,price,direction      (direction 1 = buy, -1 = sell)
//   orderbook file: ask1_price,ask1_size,bid1_price,bid1_size,ask2_price,...   one row per message
// Types: 1 new limit order, 2 partial cancel, 3 delete, 4 visible execution,
//        5 hidden execution, 6 cross trade, 7 halt.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "order_book.hpp"

namespace lobster {

struct Message {
    int type;
    ob::OrderId id;
    ob::Qty size;
    ob::Price price;
    ob::Side side;
};

inline std::string slurp(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::perror(path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(static_cast<size_t>(n), '\0');
    if (std::fread(&s[0], 1, s.size(), f) != s.size()) { std::perror(path.c_str()); std::exit(1); }
    std::fclose(f);
    return s;
}

inline std::vector<Message> read_messages(const std::string& path) {
    std::string buf = slurp(path);
    std::vector<Message> out;
    const char* p = buf.c_str();
    while (*p) {
        char* e;
        std::strtod(p, &e);  // time, unused
        if (e == p) break;
        Message m;
        m.type = static_cast<int>(std::strtol(e + 1, &e, 10));
        m.id = std::strtoull(e + 1, &e, 10);
        m.size = std::strtoll(e + 1, &e, 10);
        m.price = std::strtoll(e + 1, &e, 10);
        m.side = std::strtol(e + 1, &e, 10) == 1 ? ob::Side::Buy : ob::Side::Sell;
        out.push_back(m);
        while (*e && *e != '\n') ++e;
        p = *e ? e + 1 : e;
    }
    return out;
}

// Returns all numbers row-major; sets cols to the number of columns (4 per level).
inline std::vector<int64_t> read_orderbook(const std::string& path, size_t& cols) {
    std::string buf = slurp(path);
    std::vector<int64_t> out;
    cols = 1;
    for (const char* c = buf.c_str(); *c && *c != '\n'; ++c) cols += (*c == ',');
    const char* p = buf.c_str();
    while (*p) {
        char* e;
        int64_t v = std::strtoll(p, &e, 10);
        if (e == p) break;
        out.push_back(v);
        while (*e == ',' || *e == '\n' || *e == '\r') ++e;
        p = e;
    }
    return out;
}

}  // namespace lobster
