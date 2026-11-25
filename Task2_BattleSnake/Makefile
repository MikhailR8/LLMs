# Battlesnake C++ - survival core v1
# Build: make          -> build/battlesnake (the server)
#       make test     -> unit tests + self-play harness
# Requires: g++ (or clang++) with C++17, make. No external dependencies
# (nlohmann/json and cpp-httplib are vendored under third_party/).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
INCLUDES  = -Ithird_party -Isrc

BUILD    := build
SRV_SRC  := src/main.cpp src/rules.cpp src/strategy.cpp

.PHONY: all tests test run clean

all: $(BUILD)/battlesnake

$(BUILD)/battlesnake: $(SRV_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRV_SRC) -o $@

$(BUILD)/test_rules: tests/test_rules.cpp src/rules.cpp src/strategy.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_rules.cpp src/rules.cpp src/strategy.cpp -o $@

$(BUILD)/selfplay: tests/selfplay.cpp src/rules.cpp src/strategy.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/selfplay.cpp src/rules.cpp src/strategy.cpp -o $@

tests: $(BUILD)/test_rules $(BUILD)/selfplay

test: tests $(BUILD)/battlesnake
	./$(BUILD)/test_rules
	./$(BUILD)/selfplay --games 200 --mode duel
	./$(BUILD)/selfplay --games 100 --mode ffa

# Run the server locally on port 8080 (override with PORT=...).
run: $(BUILD)/battlesnake
	./$(BUILD)/battlesnake

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
