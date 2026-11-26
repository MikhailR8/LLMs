// Transposition-table soundness tests (v5).
//
// 1. Strict-mode equivalence: pure alpha-beta (no LMR/futility) with the TT
//    and without must return IDENTICAL root values at fixed depth. Alpha-beta
//    is pruning-order independent and the TT only supplies sound bounds, so
//    any difference is a real bug (key collision, wrong bound flag, tainted
//    store). This is the empirical half of the TT's correctness proof; the
//    other half (the key identifying positions exactly) is in
//    test_fastboard.cpp.
// 2. Determinism: identical searches after clear() return identical results.
// 3. The TT actually hits in a normal search (eval cache within one turn).
// 4. Multi-turn sequence: values stay equal turn after turn.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "search.hpp"
#include "tt.hpp"

using namespace bs;

namespace {

int g_failed = 0;
#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) {                                    \
      ++g_failed;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                       \
      std::printf("\n");                              \
      if (g_failed > 8) std::exit(1);                 \
    }                                                 \
  } while (0)

struct Rng {
  std::mt19937 eng;
  Rng(unsigned seed) : eng(seed) {}
  int operator()(int lo, int hi) { return lo + (int)(eng() % (unsigned)(hi - lo + 1)); }
};

GameState randomState(Rng& r, int W, int H, int nSnakes) {
  GameState g;
  g.width = W;
  g.height = H;
  g.foodSpawning = false;
  for (int i = 0; i < nSnakes; ++i) {
    SnakeState s;
    s.id = "s" + std::to_string(i);
    s.alive = true;
    s.health = r(20, 100);
    Point head{r(0, W - 1), r(0, H - 1)};
    s.body.push_back(head);
    Point cur = head;
    int len = r(3, 12);
    for (int k = 1; k < len; ++k) {
      for (int tries = 0; tries < 8; ++tries) {
        Dir d = (Dir)r(0, 3);
        Point n = step(cur, d);
        if (g.inBounds(n)) {
          cur = n;
          break;
        }
      }
      s.body.push_back(cur);
    }
    if (r(0, 3) == 0) s.body.push_back(s.body.back());  // duplicated tail
    g.snakes.push_back(std::move(s));
  }
  int nFood = r(0, 5);
  for (int i = 0; i < nFood; ++i) {
    Point f{r(0, W - 1), r(0, H - 1)};
    if (!g.isFoodAt(f)) g.food.push_back(f);
  }
  g.turn = r(0, 250);
  return g;
}

// Hand-built mid-game duel: 16-long snakes, food, turn 120.
GameState midgameDuel() {
  GameState g;
  g.width = 11;
  g.height = 11;
  g.turn = 120;
  g.food = {{5, 5}, {2, 8}, {8, 2}, {5, 9}};
  SnakeState me;
  me.id = "s0";
  me.health = 78;
  for (int i = 0; i < 16; ++i) me.body.push_back(Point{5 + ((i % 3) - 1), 9 - i / 3});
  SnakeState opp;
  opp.id = "opp";
  opp.health = 82;
  for (int i = 0; i < 15; ++i) opp.body.push_back(Point{6 + (i % 2), 1 + i / 2});
  g.snakes = {me, opp};
  return g;
}

SearchOptions strictOpts(int depth, bool useTT) {
  SearchOptions o;
  o.budgetMs = 5000.0;  // far more than a depth-<=4 tree needs: no timeouts
  o.maxDepth = depth;
  o.strict = true;
  o.useTT = useTT;
  return o;
}

void testStrictEquivalence() {
  std::vector<GameState> positions;
  positions.push_back(midgameDuel());
  Rng r(4242u);
  for (int i = 0; i < 40; ++i) positions.push_back(randomState(r, 7 + i % 3, 7, r(1, 3)));

  int cases = 0;
  for (size_t pi = 0; pi < positions.size(); ++pi) {
    const GameState& g = positions[pi];
    if (g.snakes.size() < 1 || !g.snakes[0].alive) continue;
    for (int depth = 1; depth <= 3; ++depth) {
      SearchResult off = searchV2(g, "s0", strictOpts(depth, false));
      SearchResult on = searchV2(g, "s0", strictOpts(depth, true));
      ++cases;
      // Depth-completion only when no legitimate shortcut fired: a forced
      // win stops the iteration early, a single legal root move skips the
      // search entirely (depth 0, value 0 on both sides).
      const bool shortcut = off.forcedWin || on.forcedWin || off.depthReached == 0 ||
                            on.depthReached == 0;
      if (!shortcut)
        CHECK(off.depthReached == depth && on.depthReached == depth,
              "pos %zu depth completed: off=%d on=%d (want %d)", pi, off.depthReached,
              on.depthReached, depth);
      CHECK(off.score == on.score, "pos %zu value mismatch depth %d: off=%.6f on=%.6f",
            pi, depth, off.score, on.score);
      CHECK(off.bestMove == on.bestMove || off.score == on.score,
            "pos %zu move differs while values differ", pi);
    }
  }
  std::printf("  strict TT-on vs TT-off: %d position-depths, values identical\n", cases);
}

void testDeterminism() {
  globalTT().clear();
  GameState g = midgameDuel();
  SearchOptions o;
  o.budgetMs = 60.0;
  o.maxDepth = 6;
  SearchResult a = searchV2(g, "s0", o);
  globalTT().clear();
  SearchResult b = searchV2(g, "s0", o);
  CHECK(a.bestMove == b.bestMove && a.score == b.score && a.depthReached == b.depthReached,
        "non-deterministic after clear(): move %d vs %d score %.3f vs %.3f depth %d vs %d",
        (int)a.bestMove, (int)b.bestMove, a.score, b.score, a.depthReached,
        b.depthReached);
  std::printf("  determinism after clear(): ok (depth %d, move %d)\n", a.depthReached,
              (int)a.bestMove);
}

void testHitsHappen() {
  globalTT().clear();
  GameState g = midgameDuel();
  SearchOptions o;
  o.budgetMs = 25.0;
  SearchResult r = searchV2(g, "s0", o);
  CHECK(r.ttHits > 0, "no TT hits in a normal 25ms search (nodes=%ld)", r.nodes);
  std::printf("  TT hits in normal search: %ld hits over %ld nodes (depth %d)\n",
              r.ttHits, r.nodes, r.depthReached);
}

void testMultiTurnSequence() {
  // Play a real 10-turn sequence; every turn's strict value must be the
  // same with and without the TT (the TT state evolves between turns for
  // the non-strict global table, but strict mode is self-contained - this
  // guards against position/key drift between conversions).
  GameState g = midgameDuel();
  std::mt19937 rng(1234);
  for (int turn = 0; turn < 10; ++turn) {
    std::vector<Dir> moves;
    for (size_t i = 0; i < g.snakes.size(); ++i) {
      if (!g.snakes[i].alive) {
        moves.push_back(Dir::Up);
        continue;
      }
      SearchResult off = searchV2(g, g.snakes[i].id, strictOpts(2, false));
      SearchResult on = searchV2(g, g.snakes[i].id, strictOpts(2, true));
      CHECK(off.score == on.score, "turn %d snake %zu value drift: %.6f vs %.6f", turn,
            i, off.score, on.score);
      moves.push_back(on.bestMove);
    }
    applyTurn(g, moves, rng);
    if (g.aliveCount() <= 1) break;
  }
  std::printf("  multi-turn strict values stay identical\n");
}

}  // namespace

int main() {
  std::printf("TT strict equivalence:\n");
  testStrictEquivalence();
  std::printf("determinism:\n");
  testDeterminism();
  std::printf("hits:\n");
  testHitsHappen();
  std::printf("multi-turn:\n");
  testMultiTurnSequence();
  if (g_failed == 0) {
    std::printf("TT tests: ALL PASSED\n");
    return 0;
  }
  std::printf("TT tests: %d FAILURES\n", g_failed);
  return 1;
}
