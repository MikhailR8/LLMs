# Battlesnake C++ - v2 minimax search (v1 survival core included)
# Build: make          -> build/battlesnake (the server, v2 by default)
#       make test     -> unit tests + self-play harness suites
# Requires: g++ (or clang++) with C++17, make. No external dependencies
# (nlohmann/json and cpp-httplib are vendored under third_party/).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
INCLUDES  = -Ithird_party -Isrc

BUILD    := build
CORE_SRC := src/rules.cpp src/strategy.cpp src/fastboard.cpp src/search.cpp src/tt.cpp
SRV_SRC  := src/main.cpp $(CORE_SRC)

.PHONY: all tests test run clean

all: $(BUILD)/battlesnake

$(BUILD)/battlesnake: $(SRV_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRV_SRC) -o $@

$(BUILD)/test_rules: tests/test_rules.cpp src/rules.cpp src/strategy.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_rules.cpp src/rules.cpp src/strategy.cpp -o $@

$(BUILD)/test_search: tests/test_search.cpp $(CORE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_search.cpp $(CORE_SRC) -o $@

$(BUILD)/test_fastboard: tests/test_fastboard.cpp src/fastboard.cpp src/rules.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_fastboard.cpp src/fastboard.cpp src/rules.cpp -o $@

$(BUILD)/test_tailaware: tests/test_tailaware.cpp src/fastboard.cpp src/rules.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_tailaware.cpp src/fastboard.cpp src/rules.cpp -o $@

$(BUILD)/test_tt: tests/test_tt.cpp $(CORE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_tt.cpp $(CORE_SRC) -o $@

$(BUILD)/selfplay: tests/selfplay.cpp $(CORE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/selfplay.cpp $(CORE_SRC) -o $@

tests: $(BUILD)/test_rules $(BUILD)/test_search $(BUILD)/test_fastboard $(BUILD)/test_tailaware $(BUILD)/test_tt $(BUILD)/selfplay

test: tests $(BUILD)/battlesnake
	./$(BUILD)/test_rules
	./$(BUILD)/test_search
	./$(BUILD)/test_fastboard
	./$(BUILD)/test_tailaware
	./$(BUILD)/test_tt
	./$(BUILD)/selfplay --games 200 --mode duel --a v1 --b baseline
	./$(BUILD)/selfplay --games 100 --mode duel --a v2 --b v1 --v2-budget-ms 25
	./$(BUILD)/selfplay --games 60  --mode duel --a v2 --b baseline --v2-budget-ms 25
	./$(BUILD)/selfplay --games 100 --mode ffa  --a v1 --b baseline
	./$(BUILD)/selfplay --games 60  --mode ffa  --a v2 --b baseline --v2-budget-ms 25

# --- benchmark against the cloned snek-two opponent (2018 champion) ---
# Prerequisite (one-time): the opponent must be cloned next to this project
# and given our vendored nlohmann/json (their submodule is empty):
#   mkdir -p ../opponents && cd ../opponents && \
#   git clone --depth 1 https://github.com/aleksiy325/snek-two.git && \
#   mkdir -p snek-two/json/single_include/nlohmann && \
#   cp ../battlesnake-cpp/third_party/nlohmann/json.hpp \
#      snek-two/json/single_include/nlohmann/
bench/snek_two_bridge: bench/snek_two_bridge.cpp
	g++ -std=c++11 -O2 -w bench/snek_two_bridge.cpp -o $@

bench-snek2: $(BUILD)/selfplay bench/snek_two_bridge
	./$(BUILD)/selfplay --games 50 --mode duel --a v2 --b snek2 --v2-budget-ms 25

# Zoo benchmark against any live Battlesnake HTTP server (current API).
# Example: make bench-remote REMOTE_URL=http://localhost:8422/move
REMOTE_URL ?= http://localhost:8423/move
bench-remote: $(BUILD)/selfplay
	./$(BUILD)/selfplay --games 15 --mode duel --a v2 --b $(REMOTE_URL) --v2-budget-ms 25

# Run the server locally on port 8080 (override with PORT=...).
run: $(BUILD)/battlesnake
	./$(BUILD)/battlesnake

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
