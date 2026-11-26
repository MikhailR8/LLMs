# Battlesnake C++ - v2 minimax search (v1 survival core included)
# Build: make          -> build/battlesnake (the server, v2 by default)
#       make test     -> unit tests + self-play harness suites
# Requires: g++ (or clang++) with C++17, make. No external dependencies
# (nlohmann/json and cpp-httplib are vendored under third_party/).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
INCLUDES  = -Ithird_party -Isrc

BUILD    := build
CORE_SRC := src/rules.cpp src/strategy.cpp src/search.cpp
SRV_SRC  := src/main.cpp $(CORE_SRC)

.PHONY: all tests test run clean

all: $(BUILD)/battlesnake

$(BUILD)/battlesnake: $(SRV_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRV_SRC) -o $@

$(BUILD)/test_rules: tests/test_rules.cpp src/rules.cpp src/strategy.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_rules.cpp src/rules.cpp src/strategy.cpp -o $@

$(BUILD)/test_search: tests/test_search.cpp $(CORE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_search.cpp $(CORE_SRC) -o $@

$(BUILD)/selfplay: tests/selfplay.cpp $(CORE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/selfplay.cpp $(CORE_SRC) -o $@

tests: $(BUILD)/test_rules $(BUILD)/test_search $(BUILD)/selfplay

test: tests $(BUILD)/battlesnake
	./$(BUILD)/test_rules
	./$(BUILD)/test_search
	./$(BUILD)/selfplay --games 200 --mode duel --a v1 --b baseline
	./$(BUILD)/selfplay --games 100 --mode duel --a v2 --b v1 --v2-budget-ms 25
	./$(BUILD)/selfplay --games 60  --mode duel --a v2 --b baseline --v2-budget-ms 25
	./$(BUILD)/selfplay --games 100 --mode ffa --a v1 --b baseline
	./$(BUILD)/selfplay --games 60  --mode ffa --a v2 --b baseline --v2-budget-ms 25

# Run the server locally on port 8080 (override with PORT=...).
run: $(BUILD)/battlesnake
	./$(BUILD)/battlesnake

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
