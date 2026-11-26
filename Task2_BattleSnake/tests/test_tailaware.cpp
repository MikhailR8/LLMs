// Ground-truth tests for the tail-aware flood fill (port of hovering-hobbs
// spread_from_head_tail_aware; the expected values below are the original's
// hand-derived fixtures, re-derived by hand for this port).
//
// 1. Sealed corner escapes through the vacating tail: 19 squares in 5 cycles.
// 2. Body squares open on schedule, not early: claimed 4/4/5 at caps 0/1/2.
// 3. A double-stacked (just-fed) tail takes the extra tick - frontier dies.
// 4. Long body squares stay walls inside the cycle budget.
// 5. Two far snakes: entered sets disjoint, both alive.
// 6. Fuzz: prefix monotonicity in cap, entered pairwise disjoint, entered
//    outside blockedFinal, everything on valid cells; single-snake states
//    agree between the multi-source fill and the single-source twin.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "fastboard.hpp"

using namespace bs;

namespace {

int g_failed = 0;
#define CHECK(cond, ...)                                                    \
  do {                                                                      \
    if (!(cond)) {                                                          \
      ++g_failed;                                                           \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
      std::printf(__VA_ARGS__);                                             \
      std::printf("\n");                                                    \
      if (g_failed > 8) std::exit(1);                                       \
    }                                                                       \
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
  g.hazardDamagePerTurn = 0;
  for (int i = 0; i < nSnakes; ++i) {
    SnakeState s;
    s.id = "s" + std::to_string(i);
    s.alive = true;
    s.health = r(1, 100);
    int x = r(0, W - 1), y = r(0, H - 1);
    Point cur{x, y};
    s.body.push_back(cur);
    int len = r(1, 6);
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
    if (r(0, 3) == 0 && s.body.size() >= 2)
      s.body.push_back(s.body.back());  // just-fed duplicated tail
    g.snakes.push_back(std::move(s));
  }
  int nFood = r(0, 6);
  for (int i = 0; i < nFood; ++i)
    g.food.push_back(Point{r(0, W - 1), r(0, H - 1)});
  return g;
}

// Explicit board: bodies head-first; 11x11 standard.
GameState makeBoard(std::vector<std::vector<Point>> bodies) {
  GameState g;
  g.width = 11;
  g.height = 11;
  g.foodSpawning = false;
  g.hazardDamagePerTurn = 0;
  for (size_t i = 0; i < bodies.size(); ++i) {
    SnakeState s;
    s.id = "s" + std::to_string(i);
    s.alive = true;
    s.health = 100;
    s.body = bodies[i];
    g.snakes.push_back(std::move(s));
  }
  return g;
}

FastState convert(const GameState& g) {
  FastState f;
  CHECK(fastFromGame(g, f, 0), "fastFromGame failed");
  return f;
}

const std::vector<Point> kSealedCorner = {
    {0, 0}, {0, 1}, {1, 1}, {1, 0}};  // head boxed in by body + tail

int claimedAt(const FastState& f, int cap, int s = 0) {
  TailAwareResult r;
  tailAwareVoronoi(f, cap, r);
  return r.claimed[s].popcount();
}

int enteredAt(const FastState& f, int cap, int s = 0) {
  TailAwareResult r;
  tailAwareVoronoi(f, cap, r);
  return r.entered[s].popcount();
}

void testSealedCornerEscape() {
  FastState f = convert(makeBoard({kSealedCorner}));
  // 4 body + 15 new squares in 5 cycles (hand-derived in the original).
  CHECK(claimedAt(f, 5) == 19, "claimed %d != 19", claimedAt(f, 5));
  CHECK(enteredAt(f, 5) == 19, "entered %d != 19", enteredAt(f, 5));
}

void testOpenOnSchedule() {
  FastState f = convert(makeBoard({kSealedCorner}));
  CHECK(claimedAt(f, 0) == 4, "cap 0 claimed %d != 4", claimedAt(f, 0));
  CHECK(claimedAt(f, 1) == 4, "cap 1 claimed %d != 4", claimedAt(f, 1));
  CHECK(claimedAt(f, 2) == 5, "cap 2 claimed %d != 5", claimedAt(f, 2));
  CHECK(enteredAt(f, 0) == 1, "cap 0 entered %d != 1", enteredAt(f, 0));
  CHECK(enteredAt(f, 1) == 2, "cap 1 entered %d != 2", enteredAt(f, 1));
  CHECK(enteredAt(f, 2) == 4, "cap 2 entered %d != 4", enteredAt(f, 2));
}

void testStackedTailDies() {
  std::vector<Point> stacked = kSealedCorner;
  stacked.push_back({1, 0});  // just-fed duplicate tail
  FastState f = convert(makeBoard({stacked}));
  for (int cap : {1, 2, 5, 20}) {
    CHECK(claimedAt(f, cap) == 4, "cap %d claimed %d != 4", cap, claimedAt(f, cap));
    CHECK(enteredAt(f, cap) == 1, "cap %d entered %d != 1", cap, enteredAt(f, cap));
  }
  // The unstacked twin escapes: 19.
  FastState u = convert(makeBoard({kSealedCorner}));
  CHECK(claimedAt(u, 5) == 19, "unstacked claimed %d != 19", claimedAt(u, 5));
}

void testLongBodyStaysWall() {
  std::vector<Point> coiled = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 2},
                               {1, 2}, {1, 3}, {0, 3}, {0, 4}, {1, 4}};
  FastState f = convert(makeBoard({coiled}));
  CHECK(claimedAt(f, 5) == 10, "claimed %d != 10", claimedAt(f, 5));
  CHECK(enteredAt(f, 5) == 1, "entered %d != 1", enteredAt(f, 5));
}

void testTwoFarSnakes() {
  FastState f = convert(makeBoard({
      {{2, 2}, {2, 1}, {2, 0}},
      {{8, 8}, {8, 9}, {8, 10}},
  }));
  TailAwareResult r;
  tailAwareVoronoi(f, 5, r);
  CHECK((r.entered[0] & r.entered[1]).none(), "entered sets overlap");
  CHECK((r.claimed[0] & r.claimed[1]).none(), "claimed sets overlap");
  CHECK(r.entered[0].popcount() >= 4, "snake 0 did not spread");
  CHECK(r.entered[1].popcount() >= 4, "snake 1 did not spread");
}

void testFuzz() {
  Rng r(20261126u);
  int cases = 0;
  for (int it = 0; it < 400; ++it) {
    const int W = r(5, 11), H = r(5, 11), n = r(1, 4);
    GameState g = randomState(r, W, H, n);
    FastState f;
    if (!fastFromGame(g, f, 0)) continue;
    ++cases;

    for (int cap : {3, 6, 20}) {
      TailAwareResult a, b;
      tailAwareVoronoi(f, cap, a);
      tailAwareVoronoi(f, cap * 2, b);
      // prefix monotonicity: doubling the cap keeps every earlier walk; the
      // claimed SUM cannot shrink (individual claimed sets may lose squares
      // to a rival that now reaches them first - the original's own caveat)
      int sumA = 0, sumB = 0;
      for (int s = 0; s < f.nSnakes; ++s) {
        CHECK((a.entered[s] & ~b.entered[s]).none(),
              "it %d cap %d: snake %d entered shrinks with bigger cap", it, cap, s);
        sumA += a.claimed[s].popcount();
        sumB += b.claimed[s].popcount();
      }
      CHECK(sumA <= sumB, "it %d cap %d: claimed sum %d > %d", it, cap, sumA, sumB);
      // entered pairwise disjoint outside head cells (the fuzz generator
      // allows overlapping bodies, so two heads may share a cell - never
      // possible in real games or post-makeTurnFast search states)
      Bits allHeads;
      for (int s = 0; s < f.nSnakes; ++s) allHeads.set(f.snakes[s].headCell());
      Bits anyEntered;
      for (int s = 0; s < f.nSnakes; ++s) {
        CHECK((a.entered[s] & ~f.geom.valid).none(), "it %d: entered off-board", it);
        // (the seeded head cell may legitimately still be a wall: it opens
        // only at cycle len)
        Bits walk = a.entered[s];
        walk.reset(f.snakes[s].headCell());
        CHECK((walk & a.blockedFinal).none(),
              "it %d: entered a never-opened body cell", it);
        for (int t = s + 1; t < f.nSnakes; ++t)
          CHECK(((a.entered[s] & a.entered[t]) & ~allHeads).none(),
                "it %d: entered sets overlap off-head", it);
        anyEntered |= a.entered[s];
      }
      CHECK((anyEntered & ~f.geom.valid).none(), "it %d: union off-board", it);
      // single-source twin must match the multi fill on one-snake states
      if (n == 1) {
        Bits single;
        tailAwareFlood(f, 0, cap, single);
        CHECK(single == a.entered[0], "it %d cap %d: single != multi", it, cap);
      }
    }
  }
  std::printf("fuzz: %d states\n", cases);
  CHECK(cases > 300, "too few fuzz cases generated");
}

}  // namespace

int main() {
  testSealedCornerEscape();
  testOpenOnSchedule();
  testStackedTailDies();
  testLongBodyStaysWall();
  testTwoFarSnakes();
  testFuzz();
  if (g_failed == 0)
    std::printf("tail-aware flood fill: all tests passed\n");
  return g_failed == 0 ? 0 : 1;
}
