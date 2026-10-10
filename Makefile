CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc
BIN      := build

all: $(BIN)/replay_lobster $(BIN)/replay_binance $(BIN)/replay_bitfinex $(BIN)/match_flow $(BIN)/test_matching

$(BIN)/%: src/%.cpp include/order_book.hpp src/lobster.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BIN)/test_matching: tests/test_matching.cpp include/order_book.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

# 1) hand-written cases  2) engine vs independent Python exchange, byte for byte
# 3) rebuild the book from the published messages and compare with every snapshot
test: all
	@mkdir -p data
	$(BIN)/test_matching
	python3 -I tools/gen_flow.py data 200000 10 1
	$(BIN)/match_flow data/flow.csv data/out_message.csv data/out_orderbook.csv 10
	cmp data/out_message.csv data/ref_message.csv && cmp data/out_orderbook.csv data/ref_orderbook.csv && echo "engine output identical to reference"
	$(BIN)/replay_lobster data/ref_message.csv data/ref_orderbook.csv

clean:
	rm -rf $(BIN) data/*.csv

.PHONY: all test clean
