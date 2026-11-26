// Differential tests for the FastState bitboard engine (v3 hot path).
//
// 1. makeTurnFast vs applyTurn on random states/moves: post-turn state must
//    match exactly (alive flags, healths, bodies of survivors, food, turn).
// 2. unmakeTurnFast: the state must be restored bit-for-bit (all fields).
// 3. floodFill/voronoi vs naive per-cell BFS references.
// 4. Shift primitives on small boards.
//
// The gold engine runs with foodSpawning = false, matching the search's
// operating point (and makeTurnFast never spawns food).
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "fastboard.hpp"

using namespace bs;

namespace {

int g_failed = 0;
bool g_quiet = false;  // dumps the first differential case in full
#define CHECK(cond, ...)                       \
  do {                                         \
    if (!(cond)) {                             \
      ++g_failed;                              \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                \
      std::printf("\n");                       \
      if (g_failed > 8) std::exit(1);          \
    }                                          \
  } while (0)

// ------------------------- random state generation ------------------------

struct Rng {
  std::mt19937 eng;
  Rng(unsigned seed) : eng(seed) {}
  int operator()(int lo, int hi) { return lo + (int)(eng() % (unsigned)(hi - lo + 1)); }
};

GameState randomState(Rng& r, int W, int H, int nSnakes, bool withHazards) {
  GameState g;
  g.width = W;
  g.height = H;
  g.foodSpawning = false;
  g.hazardDamagePerTurn = withHazards ? 2 : 0;
  for (int i = 0; i < nSnakes; ++i) {
    SnakeState s;
    s.id = "s" + std::to_string(i);
    s.alive = true;
    s.health = r(1, 100);
    // random walk body, 1..6 segments, head-first
    int x = r(0, W - 1), y = r(0, H - 1);
    Point head{x, y};
    s.body.push_back(head);
    Point cur = head;
    int len = r(1, 6);
    for (int k = 1; k < len; ++k) {
      // step to a random in-bounds neighbor (no self-overlap guarantee:
      // overlapping bodies are fine, applyTurn handles them)
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
    // sometimes append a duplicated tail (the "grew last turn" shape)
    if (r(0, 3) == 0 && s.body.size() >= 2)
      s.body.push_back(s.body.back());
    if (r(0, 9) == 0) s.health = 1;  // about to starve
    g.snakes.push_back(std::move(s));
  }
  int nFood = r(0, 6);
  for (int i = 0; i < nFood; ++i) {
    Point f{r(0, W - 1), r(0, H - 1)};
    if (!g.isFoodAt(f)) g.food.push_back(f);
  }
  if (withHazards) {
    int nH = r(0, 4);
    for (int i = 0; i < nH; ++i) {
      Point h{r(0, W - 1), r(0, H - 1)};
      if (!g.isHazardAt(h)) g.hazards.push_back(h);
    }
  }
  g.turn = r(0, 300);
  return g;
}

void compareStates(const GameState& gold, const GameState& fast, const char* what) {
  CHECK(gold.turn == fast.turn, "%s turn %d vs %d", what, gold.turn, fast.turn);
  CHECK(gold.snakes.size() == fast.snakes.size(), "%s snake count", what);
  for (size_t i = 0; i < gold.snakes.size(); ++i) {
    const SnakeState& a = gold.snakes[i];
    const SnakeState& b = fast.snakes[i];
    CHECK(a.alive == b.alive, "%s snake %zu alive %d vs %d", what, i, (int)a.alive,
          (int)b.alive);
    CHECK(a.health == b.health, "%s snake %zu health %d vs %d", what, i, a.health, b.health);
    if (!a.alive) continue;
    CHECK(a.body.size() == b.body.size(), "%s snake %zu len %zu vs %zu", what, i,
          a.body.size(), b.body.size());
    for (size_t k = 0; k < a.body.size() && k < b.body.size(); ++k)
      CHECK(a.body[k] == b.body[k], "%s snake %zu seg %zu (%d,%d) vs (%d,%d)", what, i, k,
            a.body[k].x, a.body[k].y, b.body[k].x, b.body[k].y);
  }
  // food as sets
  CHECK(gold.food.size() == fast.food.size(), "%s food count %zu vs %zu", what,
        gold.food.size(), fast.food.size());
  for (const Point& f : gold.food)
    CHECK(fast.isFoodAt(f), "%s food (%d,%d) missing", what, f.x, f.y);
  for (const Point& f : fast.food)
    CHECK(gold.isFoodAt(f), "%s extra food (%d,%d)", what, f.x, f.y);
}

bool statesIdentical(const FastState& a, const FastState& b) {
  if (a.turn != b.turn || a.nSnakes != b.nSnakes || a.hazardDamage != b.hazardDamage)
    return false;
  if (a.key != b.key) return false;
  if (!(a.occ == b.occ) || !(a.food == b.food) || !(a.hazards == b.hazards)) return false;
  for (int i = 0; i < a.nSnakes; ++i) {
    if (!(a.bodyB[i] == b.bodyB[i])) return false;
    const FastSnake& x = a.snakes[i];
    const FastSnake& y = b.snakes[i];
    if (x.headIdx != y.headIdx || x.len != y.len || x.health != y.health ||
        x.alive != y.alive || x.dirty != y.dirty || x.seqH != y.seqH)
      return false;
    // Ring slots outside the body span are scratch (the slot the turn wrote
    // sits beyond the restored span); only the span itself is semantic.
    for (int k = 0; k < x.len; ++k)
      if (x.ring[(x.headIdx + k) & kRingMask] != y.ring[(y.headIdx + k) & kRingMask])
        return false;
  }
  return true;
}

// ------------------------------ naive BFS refs ----------------------------

std::vector<int> naiveDist(const GameState& g, const std::vector<uint8_t>& blocked,
                           const Point& start) {
  std::vector<int> dist(g.width * g.height, -1);
  std::vector<Point> q;
  dist[start.y * g.width + start.x] = 0;
  q.push_back(start);
  size_t head = 0;
  while (head < q.size()) {
    Point cur = q[head++];
    for (int d = 0; d < kNumDirs; ++d) {
      Point nxt = step(cur, (Dir)d);
      if (!g.inBounds(nxt)) continue;
      int i = nxt.y * g.width + nxt.x;
      if (blocked[i] || dist[i] != -1) continue;
      dist[i] = dist[cur.y * g.width + cur.x] + 1;
      q.push_back(nxt);
    }
  }
  return dist;
}

std::vector<uint8_t> blockedVec(const GameState& g) {
  std::vector<uint8_t> blocked(g.width * g.height, 0);
  for (const Point& p : blockedAfterMove(g)) blocked[p.y * g.width + p.x] = 1;
  return blocked;
}

// --------------------------------- tests -----------------------------------

void testShifts() {
  BitGeom g = BitGeom::make(5, 4);  // 20 cells
  Bits one;
  one.clearAll();
  one.set(g.idx(2, 1));
  Bits up = moveUp(one, g);
  CHECK(up.popcount() == 1 && up.test(g.idx(2, 2)), "up shift");
  Bits down = moveDown(one, g);
  CHECK(down.popcount() == 1 && down.test(g.idx(2, 0)), "down shift");
  Bits left = moveLeft(one, g);
  CHECK(left.popcount() == 1 && left.test(g.idx(1, 1)), "left shift");
  Bits right = moveRight(one, g);
  CHECK(right.popcount() == 1 && right.test(g.idx(3, 1)), "right shift");

  // edges: left from column 0 must vanish, not wrap
  Bits c0;
  c0.clearAll();
  c0.set(g.idx(0, 2));
  CHECK(moveLeft(c0, g).none(), "left off column 0");
  Bits c4;
  c4.clearAll();
  c4.set(g.idx(4, 2));
  CHECK(moveRight(c4, g).none(), "right off column 4");
  Bits top;
  top.clearAll();
  top.set(g.idx(3, 3));
  CHECK(moveUp(top, g).none(), "up off top row");
  Bits bottom;
  bottom.clearAll();
  bottom.set(g.idx(3, 0));
  CHECK(moveDown(bottom, g).none(), "down off bottom row");

  // wrap check: bit at (0,1) shifted down lands at (0,0), not (4,0)
  Bits w;
  w.clearAll();
  w.set(g.idx(0, 1));
  Bits wd = moveDown(w, g);
  CHECK(wd.popcount() == 1 && wd.test(g.idx(0, 0)), "down no wrap");

  // full-board flood from one corner on an empty board
  Bits all;
  all.clearAll();
  Bits startBit;
  startBit.clearAll();
  startBit.set(g.idx(0, 0));
  FloodResult fr = floodFill(g, all, startBit, nullptr);
  CHECK(fr.reached.popcount() == 20, "empty board flood reaches all, got %d",
        fr.reached.popcount());
}

void dumpGame(const GameState& g, const char* tag) {
  std::printf("  %s turn=%d food=[", tag, g.turn);
  for (const Point& f : g.food) std::printf("(%d,%d) ", f.x, f.y);
  std::printf("] hz=[");
  for (const Point& f : g.hazards) std::printf("(%d,%d) ", f.x, f.y);
  std::printf("]\n");
  for (size_t i = 0; i < g.snakes.size(); ++i) {
    const SnakeState& s = g.snakes[i];
    std::printf("    s%zu alive=%d hp=%d body=[", i, (int)s.alive, s.health);
    for (const Point& b : s.body) std::printf("(%d,%d) ", b.x, b.y);
    std::printf("]\n");
  }
}

void dumpFast(const FastState& f, const char* tag) {
  std::printf("  %s turn=%d food bits: ", tag, f.turn);
  for (int i = 0; i < f.geom.cells; ++i)
    if (f.food.test(i)) std::printf("(%d,%d) ", f.geom.xOf(i), f.geom.yOf(i));
  std::printf("\n");
  for (int i = 0; i < f.nSnakes; ++i) {
    const FastSnake& s = f.snakes[i];
    std::printf("    s%d alive=%d hp=%d len=%d body=[", i, (int)s.alive, s.health, s.len);
    for (int k = 0; k < s.len; ++k)
      std::printf("(%d,%d) ", f.geom.xOf(s.cellAt(k)), f.geom.yOf(s.cellAt(k)));
    std::printf("]\n");
  }
}

void testMakeTurnDifferential() {
  const int kCases = 3000;
  int multiFood = 0, elimCase = 0;
  for (int t = 0; t < kCases; ++t) {
    Rng r(t * 7919u + 13u);
    const int W = r(4, 9), H = r(4, 9);
    const int nSnakes = r(1, 4);
    const bool hazards = (r(0, 5) == 0);
    GameState g = randomState(r, W, H, nSnakes, hazards);

    // random moves for all snakes
    Dir moves[kMaxFastSnakes] = {Dir::Up};
    std::vector<Dir> goldMoves(g.snakes.size(), Dir::Up);
    for (size_t i = 0; i < g.snakes.size(); ++i) {
      Dir d = (Dir)r(0, 3);
      moves[i] = d;
      goldMoves[i] = d;
    }

    FastState f, fBefore;
    CHECK(fastFromGame(g, f), "conversion");
    fBefore = f;

    std::mt19937 rngDummy(0);
    GameState gold = g;
    applyTurn(gold, goldMoves, rngDummy);

    TurnUndo u;
    makeTurnFast(f, moves, u);
    // Incremental Zobrist key must equal the from-scratch derivation after
    // every turn (movement, feeding, hazards, eliminations, tick, turn++).
    if (f.key != computePositionKey(f, 0) && !g_quiet) {
      g_quiet = true;
      std::printf("KEY DIVERGENCE case %d PRE:\n", t);
      dumpGame(g, "pre");
      std::printf("  moves:");
      for (size_t i = 0; i < g.snakes.size(); ++i)
        std::printf(" %zu=%s", i, dirToString(moves[i]));
      std::printf("\n  POST-FAST:\n");
      dumpFast(f, "fast");
    }
    CHECK(f.key == computePositionKey(f, 0), "incremental key case %d", t);
    if (gold.food.size() > 1) ++multiFood;
    bool anyElim = false;
    for (const auto& s : gold.snakes)
      if (!s.alive) anyElim = true;
    if (anyElim) ++elimCase;

    GameState fastOut;
    fastToGame(f, g, fastOut);
    const int before = g_failed;
    compareStates(gold, fastOut, "post-turn");
    if (g_failed > before && !g_quiet) {
      g_quiet = true;
      std::printf("CASE %d PRE:\n", t);
      dumpGame(g, "pre-gold");
      std::printf("  moves:");
      for (size_t i = 0; i < g.snakes.size(); ++i)
        std::printf(" %zu=%s", i, dirToString(moves[i]));
      std::printf("\n  POST-GOLD:\n");
      dumpGame(gold, "gold");
      std::printf("  POST-FAST:\n");
      dumpFast(f, "fast");
    }

    // unmake must restore the exact pre-turn state
    unmakeTurnFast(f, u);
    CHECK(statesIdentical(f, fBefore), "unmake restore case %d", t);

    // and a second make from the restored state must be reproducible
    TurnUndo u2;
    makeTurnFast(f, moves, u2);
    GameState fastOut2;
    fastToGame(f, g, fastOut2);
    compareStates(gold, fastOut2, "remake");
    unmakeTurnFast(f, u2);
  }
  std::printf("  makeTurn differential: %d cases (%d with eliminations, %d multi-food) ok\n",
              kCases, elimCase, multiFood);
}

void testFloodAndVoronoiVsNaive() {
  const int kCases = 800;
  for (int t = 0; t < kCases; ++t) {
    Rng r(t * 104729u + 7u);
    const int W = r(4, 9), H = r(4, 9);
    GameState g = randomState(r, W, H, r(1, 4), false);
    FastState f;
    CHECK(fastFromGame(g, f), "conversion");
    const Bits blocked = f.blockedAfterMoveBits();

    // floodFill from my head vs naive dist
    for (int s = 0; s < f.nSnakes; ++s) {
      if (!f.snakes[s].alive) continue;
      const SnakeState& gs = g.snakes[s];
      std::vector<int> dist = naiveDist(g, blockedVec(g), gs.head());
      Bits startBit;
      startBit.clearAll();
      startBit.set(f.geom.idx(gs.head().x, gs.head().y));
      FloodResult fr = floodFill(f.geom, blocked, startBit, &f.food);
      int reach = 0;
      for (int i = 0; i < f.geom.cells; ++i)
        if (dist[i] >= 0) ++reach;
      CHECK(fr.reached.popcount() == reach, "flood reach %d vs %d", fr.reached.popcount(),
            reach);
      // firstHit must equal min dist over food
      int minFood = -1;
      for (const Point& fd : g.food) {
        int d = dist[fd.y * W + fd.x];
        if (d >= 0 && (minFood < 0 || d < minFood)) minFood = d;
      }
      CHECK(fr.firstHit == minFood, "firstHit %d vs naive %d", fr.firstHit, minFood);
      // per-cell reached must match exactly
      for (int i = 0; i < f.geom.cells; ++i)
        CHECK(fr.reached.test(i) == (dist[i] >= 0), "flood cell %d mismatch", i);
    }

    // voronoi vs naive per-snake ownership
    {
      std::vector<std::vector<int>> dists;
      std::vector<int> aliveIdx;
      for (int s = 0; s < f.nSnakes; ++s) {
        if (!f.snakes[s].alive) continue;
        aliveIdx.push_back(s);
        dists.push_back(naiveDist(g, blockedVec(g), g.snakes[s].head()));
      }
      Bits heads[kMaxFastSnakes];
      for (size_t k = 0; k < aliveIdx.size(); ++k) {
        heads[k].clearAll();
        heads[k].set(f.geom.idx(g.snakes[aliveIdx[k]].head().x,
                                g.snakes[aliveIdx[k]].head().y));
      }
      VoronoiResult vr;
      voronoi(f.geom, blocked, heads, (int)aliveIdx.size(), vr);
      for (size_t k = 0; k < aliveIdx.size(); ++k) {
        const int s = aliveIdx[k];
        for (int i = 0; i < f.geom.cells; ++i) {
          // naive owner: strictly smallest dist among alive snakes
          int owner = -1;  // -1 none, -2 tie
          int best = -1;
          for (size_t m = 0; m < aliveIdx.size(); ++m) {
            int d = dists[m][i];
            if (d < 0) continue;
            if (best < 0 || d < best) {
              best = d;
              owner = (int)m;
            } else if (d == best && owner != (int)m) {
              owner = -2;
            }
          }
          const bool claimedByS = vr.claimed[s].test(i);
          if (claimedByS != (owner == (int)k) && !g_quiet) {
            g_quiet = true;
            std::printf(
                "VORONOI DIVERGENCE case %d: cell %d (%d,%d) W=%d H=%d snake k=%zu\n",
                t, i, f.geom.xOf(i), f.geom.yOf(i), W, H, k);
            for (size_t m = 0; m < aliveIdx.size(); ++m)
              std::printf("  dist[%zu]=%d head=(%d,%d)\n", m, dists[m][i],
                          g.snakes[aliveIdx[m]].head().x, g.snakes[aliveIdx[m]].head().y);
            std::printf("  blocked: ");
            for (int c = 0; c < f.geom.cells; ++c)
              if (blocked.test(c)) std::printf("(%d,%d) ", f.geom.xOf(c), f.geom.yOf(c));
            std::printf("\n  bodies:\n");
            dumpGame(g, "pre");
          }
          CHECK(claimedByS == (owner == (int)k),
                "voronoi cell %d snake %d: fast=%d naive=%d", i, s, (int)claimedByS,
                owner);
        }
      }
    }
  }
  std::printf("  flood/voronoi vs naive BFS: 800 cases ok\n");
}

void testZobrist() {
  // Same random state, converted twice with the same viewer: same key.
  Rng r(20261126u);
  GameState g = randomState(r, 7, 7, 2, false);
  FastState f1, f2;
  CHECK(fastFromGame(g, f1, 0), "conv1");
  CHECK(fastFromGame(g, f2, 0), "conv2");
  CHECK(f1.key == f2.key, "deterministic key");
  CHECK(f1.key == computePositionKey(f1, 0), "reference key");

  // Viewer salt: same position, different meIdx -> different key.
  FastState f3;
  CHECK(fastFromGame(g, f3, 1), "conv3");
  CHECK(f3.key != f1.key, "me salt");
  CHECK(f3.key == computePositionKey(f3, 1), "reference key me1");

  // Board salt: same local shape on a different board size.
  GameState g2 = g;
  g2.width = 9;
  g2.height = 9;
  FastState f4;
  CHECK(fastFromGame(g2, f4, 0), "conv4");
  CHECK(f4.key != f1.key, "board salt");

  // Single-component perturbations must change the key: body order, food,
  // health, turn, duplication.
  {
    GameState p = g;
    // swap two mid-body cells (same set, different sequence)
    if (p.snakes[0].body.size() >= 4) {
      std::swap(p.snakes[0].body[1], p.snakes[0].body[2]);
      FastState fp;
      CHECK(fastFromGame(p, fp, 0), "conv perm");
      CHECK(fp.key != f1.key, "body order changes key");
    }
  }
  {
    GameState p = g;
    p.food.push_back(Point{0, 0});
    FastState fp;
    CHECK(fastFromGame(p, fp, 0), "conv food");
    CHECK(fp.key != f1.key, "food changes key");
  }
  {
    GameState p = g;
    p.snakes[0].health += 1;
    FastState fp;
    CHECK(fastFromGame(p, fp, 0), "conv health");
    CHECK(fp.key != f1.key, "health changes key");
  }
  {
    GameState p = g;
    p.turn += 1;
    FastState fp;
    CHECK(fastFromGame(p, fp, 0), "conv turn");
    CHECK(fp.key != f1.key, "turn changes key");
  }
  {
    GameState p = g;
    p.snakes[0].body.push_back(p.snakes[0].body.back());  // duplicated tail
    FastState fp;
    CHECK(fastFromGame(p, fp, 0), "conv dup");
    CHECK(fp.key != f1.key, "duplication changes key");
  }

  // Long chains of make/unmake keep the key exact (recompute every ply).
  Rng r2(777u);
  for (int t = 0; t < 200; ++t) {
    GameState g0 = randomState(r2, 8, 8, 2, false);
    FastState st;
    CHECK(fastFromGame(g0, st, 0), "chain conv");
    std::vector<TurnUndo> undos;
    for (int ply = 0; ply < 12; ++ply) {
      if (st.aliveCount() == 0) break;
      Dir mv[kMaxFastSnakes] = {Dir::Up};
      for (int i = 0; i < st.nSnakes; ++i)
        if (st.snakes[i].alive) mv[i] = (Dir)r2(0, 3);
      undos.emplace_back();
      makeTurnFast(st, mv, undos.back());
      CHECK(st.key == computePositionKey(st, 0), "chain key ply %d case %d", ply, t);
    }
    while (!undos.empty()) {
      unmakeTurnFast(st, undos.back());
      undos.pop_back();
    }
    CHECK(st.key == computePositionKey(st, 0), "chain key after unwind case %d", t);
    CHECK(statesIdentical(st, [&] { FastState b; fastFromGame(g0, b, 0); return b; }()),
          "chain full restore case %d", t);
  }
  std::printf("  zobrist key properties: ok\n");
}

}  // namespace

int main() {
  std::printf("shift primitives:\n");
  testShifts();
  std::printf("makeTurn differential + unmake:\n");
  testMakeTurnDifferential();
  std::printf("flood/voronoi:\n");
  testFloodAndVoronoiVsNaive();
  std::printf("zobrist key:\n");
  testZobrist();
  if (g_failed == 0) {
    std::printf("fastboard tests: ALL PASSED\n");
    return 0;
  }
  std::printf("fastboard tests: %d FAILURES\n", g_failed);
  return 1;
}
