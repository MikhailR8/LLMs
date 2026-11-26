// Unit tests for the v2 search (paranoid minimax + iterative deepening).
// Focus: the search must (a) see what v1 cannot (traps, forced exchanges),
// (b) never return an illegal move, (c) degrade safely under tiny budgets.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "rules.hpp"
#include "search.hpp"
#include "strategy.hpp"

using namespace bs;

static int g_total = 0;
static int g_passed = 0;
#define CHECK(...)                                                        \
  do {                                                                    \
    ++g_total;                                                            \
    if ((__VA_ARGS__)) {                                                  \
      ++g_passed;                                                         \
    } else {                                                              \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
    }                                                                     \
  } while (0)

static SnakeState mkSnake(const std::string& id, std::vector<Point> body, int health = 100) {
  SnakeState s;
  s.id = id;
  s.body = std::move(body);
  s.health = health;
  return s;
}

static GameState mkState(std::vector<SnakeState> snakes) {
  GameState g;
  g.width = 11;
  g.height = 11;
  g.snakes = std::move(snakes);
  return g;
}

// The exact legality filter the strategies guarantee: in bounds and not on
// any cell that stays occupied after the simultaneous move.
static bool isLegalMove(const GameState& g, const std::string& id, Dir d) {
  const int meIdx = g.indexOf(id);
  if (meIdx < 0 || !g.snakes[meIdx].alive) return false;
  const Point nh = step(g.snakes[meIdx].head(), d);
  if (!g.inBounds(nh)) return false;
  for (const Point& p : blockedAfterMove(g))
    if (p == nh) return false;
  return true;
}

int main() {
  const double kBudget = 100.0;  // ms, comfortable for unit test positions

  // --- 1. Forced move shortcut: only one legal move, no search needed. ---
  {
    GameState g = mkState({mkSnake("me", {{0, 0}, {1, 0}, {2, 0}}),
                           mkSnake("e", {{8, 8}, {8, 9}, {8, 10}})});
    SearchResult r = searchV2(g, "me", kBudget);
    CHECK(r.bestMove == Dir::Up);  // right is our neck, left/down are walls
    CHECK(!r.fellBackToV1);
  }

  // --- 2. Starving snake goes to food. ---
  {
    GameState g = mkState({mkSnake("me", {{5, 5}, {5, 4}, {5, 3}}, 12),
                           mkSnake("e", {{0, 10}, {0, 9}, {0, 8}})});
    g.food.push_back({5, 8});
    Dir d = chooseMoveV2(g, "me", kBudget);
    CHECK(d == Dir::Up);
  }

  // --- 3. Trap avoidance: a 3-cell pocket sealed by our own body with
  //        bait food inside; our length is 12, so entering is fatal. ---
  {
    GameState g = mkState({mkSnake(
        "me",
        {{5, 5}, {5, 4}, {5, 6}, {6, 3}, {7, 3}, {7, 4}, {7, 5}, {7, 6}, {7, 7},
         {6, 7}, {6, 8}, {7, 8}}),
        mkSnake("e", {{0, 10}, {0, 9}, {0, 8}})});
    g.food.push_back({6, 4});  // bait inside the pocket
    Dir d = chooseMoveV2(g, "me", kBudget);
    // Only Left (open board) and Right (into the pocket) are legal.
    CHECK(d == Dir::Left);
  }

  // --- 4. Never moves into a losing head-to-head when alternatives exist. ---
  {
    // Enemy head two cells right, longer: if we step Right and it steps
    // Left, we lose the head-to-head (5 >= 3). Paranoid search sees it.
    GameState g = mkState({mkSnake("me", {{5, 5}, {5, 4}, {5, 3}}),
                           mkSnake("e", {{7, 5}, {8, 5}, {9, 5}, {9, 6}, {9, 7}})});
    Dir d = chooseMoveV2(g, "me", kBudget);
    CHECK(d != Dir::Right);
    CHECK(isLegalMove(g, "me", d));
  }

  // --- 5. Timeout safety: a microscopic budget must still yield a legal
  //        move (falls back to v1 or the first completed iteration). ---
  {
    GameState g = mkState({mkSnake("me", {{5, 5}, {5, 4}, {5, 3}, {5, 2}}),
                           mkSnake("e", {{5, 9}, {6, 9}, {7, 9}, {8, 9}})});
    for (int i = 0; i < 4; ++i) g.food.push_back({2 + i, 2});
    for (int trial = 0; trial < 20; ++trial) {
      SearchResult r = searchV2(g, "me", 0.001);  // ~no time at all
      CHECK(isLegalMove(g, "me", r.bestMove));
    }
  }

  // --- 6. Doomed state (no legal moves) answers without crashing. ---
  {
    GameState g = mkState({mkSnake("me", {{5, 5}, {5, 4}, {5, 6}, {4, 5}, {6, 5}}),
                           mkSnake("e", {{0, 0}, {1, 0}, {2, 0}})});
    Dir d = chooseMoveV2(g, "me", kBudget);
    (void)d;  // any answer is fine; must not crash or hang
    CHECK(true);
  }

  // --- 7. Depth sanity: a normal duel position should reach depth >= 3
  //        inside 100 ms (C++ is fast; this guards against perf regressions). ---
  {
    GameState g = mkState({mkSnake("me", {{4, 5}, {4, 4}, {4, 3}, {4, 2}}),
                           mkSnake("e", {{6, 5}, {6, 6}, {6, 7}, {6, 8}})});
    g.food.push_back({5, 5});
    g.food.push_back({2, 2});
    SearchResult r = searchV2(g, "me", kBudget);
    CHECK(r.depthReached >= 3);
    std::printf("  depth sanity: depth=%d elapsed=%.1fms\n", r.depthReached, r.elapsedMs);
  }

  // --- 8. Fuzzed legality: random 2-4 snake positions, small budget; the
  //        returned move must always pass the legality filter. ---
  {
    std::mt19937 rng(987654321);
    int illegal = 0;
    int noLegal = 0;
    for (int iter = 0; iter < 60; ++iter) {
      GameState g;
      g.width = g.height = 11;
      const int nSnakes = 2 + static_cast<int>(rng() % 3);
      for (int s = 0; s < nSnakes; ++s) {
        SnakeState sn;
        sn.id = (s == 0) ? "me" : ("e" + std::to_string(s));
        Point p{static_cast<int>(rng() % 11), static_cast<int>(rng() % 11)};
        const int len = 3 + static_cast<int>(rng() % 13);
        for (int i = 0; i < len; ++i) {
          sn.body.push_back(p);
          for (int tries = 0; tries < 12; ++tries) {
            Point q = step(p, static_cast<Dir>(rng() % 4));
            if (g.inBounds(q)) { p = q; break; }
          }
        }
        sn.health = 1 + static_cast<int>(rng() % 100);
        g.snakes.push_back(std::move(sn));
      }
      for (int f = 0; f < 5; ++f) {
        Point p{static_cast<int>(rng() % 11), static_cast<int>(rng() % 11)};
        if (!g.isBodyAt(p) && !g.isFoodAt(p)) g.food.push_back(p);
      }
      if (g.indexOf("me") < 0 || !g.snakes[g.indexOf("me")].alive) continue;
      // Does any legal move exist?
      bool anyLegal = false;
      for (int d = 0; d < kNumDirs; ++d)
        if (isLegalMove(g, "me", static_cast<Dir>(d))) anyLegal = true;
      if (!anyLegal) { ++noLegal; continue; }
      const SearchResult r = searchV2(g, "me", 5.0);
      if (!isLegalMove(g, "me", r.bestMove)) ++illegal;
    }
    CHECK(illegal == 0);
    std::printf("  fuzz: 60 states, %d with no legal move, %d illegal answers\n",
                noLegal, illegal);
  }

  std::printf("search tests: %d/%d passed\n", g_passed, g_total);
  return g_passed == g_total ? 0 : 1;
}
