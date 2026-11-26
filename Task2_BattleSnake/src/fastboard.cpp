#include "fastboard.hpp"

#include <algorithm>

namespace bs {

namespace {

// Zobrist tables for the position key. Fixed seed: the hash is deterministic
// across runs (identical games replay identically), 64-bit keys with the
// usual engine-grade collision odds (~2^-64 per position pair).
struct ZobristTables {
  uint64_t w[kMaxFastSnakes][kMaxBoardCells];  // body cell at exponent i
  uint64_t betaPow[kMaxBoardCells + 64];       // beta^i, i = body position
  uint64_t beta;
  uint64_t health[kMaxFastSnakes][128];
  uint64_t foodC[kMaxBoardCells];
  uint64_t hazardC[kMaxBoardCells];
  uint64_t turnC[2048];
  uint64_t meSalt[kMaxFastSnakes];
  uint64_t hdSalt[16];

  static uint64_t splitmix(uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  ZobristTables() {
    uint64_t st = 0x1234567890ABCDEFull;
    for (auto& row : w)
      for (auto& v : row) v = splitmix(st);
    for (auto& v : betaPow) v = 0;
    // beta must be odd so it is invertible mod 2^64
    beta = splitmix(st) | 1ull;
    betaPow[0] = 1;
    for (size_t i = 1; i < sizeof(betaPow) / sizeof(betaPow[0]); ++i)
      betaPow[i] = betaPow[i - 1] * beta;
    for (auto& row : health)
      for (auto& v : row) v = splitmix(st);
    for (auto& v : foodC) v = splitmix(st);
    for (auto& v : hazardC) v = splitmix(st);
    for (auto& v : turnC) v = splitmix(st);
    for (auto& v : meSalt) v = splitmix(st);
    for (auto& v : hdSalt) v = splitmix(st);
  }
};
const ZobristTables zt;

inline int hltIdx(int h) { return h < 0 ? 0 : (h > 100 ? 100 : h); }

inline uint64_t boardSalt(int W, int H) {
  uint64_t x = 0x9E3779B97F4A7C15ull ^ (uint64_t(W) << 32) ^ uint64_t(H);
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// Removes a snake's whole component from the key (alive -> dead).
inline void removeSnakeComponent(FastState& f, int i) {
  f.key ^= f.snakes[i].seqH ^ zt.health[i][hltIdx(f.snakes[i].health)];
}

}  // namespace

Bits FastState::blockedAfterMoveBits() const {
  Bits out;
  out.clearAll();
  for (int i = 0; i < nSnakes; ++i) {
    const FastSnake& s = snakes[i];
    if (!s.alive || s.len == 0) continue;
    if (s.len == 1) continue;  // single-segment body fully vacates
    if (s.dirty) {
      // repeated cells defeat bit clearing: set body[0..len-2] exactly
      for (int k = 0; k + 1 < s.len; ++k) out.set(s.cellAt(k));
      continue;
    }
    if (s.tailDuplicated())
      out |= bodyB[i];  // tail stays: whole body blocks
    else {
      Bits b = bodyB[i];
      b.reset(s.tailCell());
      out |= b;
    }
  }
  return out;
}

bool fastFromGame(const GameState& g, FastState& out, int meIdx) {
  const int cells = g.width * g.height;
  if (cells > kMaxBoardCells) return false;
  if (static_cast<int>(g.snakes.size()) > kMaxFastSnakes) return false;
  out = FastState{};
  out.geom = BitGeom::make(g.width, g.height);
  out.nSnakes = static_cast<int>(g.snakes.size());
  out.turn = g.turn;
  out.hazardDamage = g.hazardDamagePerTurn;
  for (const Point& p : g.food) out.food.set(out.geom.idx(p.x, p.y));
  for (const Point& p : g.hazards) out.hazards.set(out.geom.idx(p.x, p.y));
  for (int i = 0; i < out.nSnakes; ++i) {
    const SnakeState& src = g.snakes[i];
    FastSnake& dst = out.snakes[i];
    dst.headIdx = 0;
    dst.len = static_cast<int>(src.body.size());
    dst.health = src.health;
    dst.alive = src.alive;
    for (size_t k = 0; k < src.body.size(); ++k)
      dst.ring[k] = static_cast<uint16_t>(out.geom.idx(src.body[k].x, src.body[k].y));
    // rolling sequence hash: head at exponent 0
    for (int k = 0; k < dst.len; ++k)
      dst.seqH += zt.w[i][dst.ring[k]] * zt.betaPow[k];
    // Dirty = repeated cell anywhere except an adjacent tail duplicate.
    Bits seen;
    for (int k = 0; k < dst.len; ++k) {
      const int c = dst.ring[k];
      if (seen.test(c)) {
        if (!(k == dst.len - 1 && k >= 1 && c == dst.ring[k - 1])) dst.dirty = true;
      } else {
        seen.set(c);
      }
    }
    if (src.alive)
      for (size_t k = 0; k < src.body.size(); ++k)
        out.bodyB[i].set(dst.ring[k]);
  }
  out.occ.clearAll();
  for (int i = 0; i < out.nSnakes; ++i)
    if (out.snakes[i].alive) out.occ |= out.bodyB[i];
  out.key = computePositionKey(out, meIdx);
  return true;
}

uint64_t computePositionKey(const FastState& f, int meIdx) {
  uint64_t k = boardSalt(f.geom.W, f.geom.H) ^ zt.meSalt[meIdx & 7] ^
               zt.hdSalt[f.hazardDamage & 15] ^ zt.turnC[f.turn & 2047];
  Bits tmp = f.food;
  for (int wi = 0; wi < kMaxBitWords; ++wi) {
    uint64_t wv = tmp.w[wi];
    while (wv) {
      k ^= zt.foodC[(wi << 6) + __builtin_ctzll(wv)];
      wv &= wv - 1;
    }
  }
  tmp = f.hazards;
  for (int wi = 0; wi < kMaxBitWords; ++wi) {
    uint64_t wv = tmp.w[wi];
    while (wv) {
      k ^= zt.hazardC[(wi << 6) + __builtin_ctzll(wv)];
      wv &= wv - 1;
    }
  }
  for (int i = 0; i < f.nSnakes; ++i)
    if (f.snakes[i].alive)
      k ^= f.snakes[i].seqH ^ zt.health[i][hltIdx(f.snakes[i].health)];
  return k;
}

void fastToGame(const FastState& f, const GameState& tpl, GameState& out) {
  out = tpl;  // keeps ids, ruleset knobs, hazards
  out.turn = f.turn;
  out.hazardDamagePerTurn = f.hazardDamage;
  out.food.clear();
  out.hazards.clear();
  for (int idx = 0; idx < f.geom.cells; ++idx) {
    Point p{f.geom.xOf(idx), f.geom.yOf(idx)};
    if (f.food.test(idx)) out.food.push_back(p);
    if (f.hazards.test(idx)) out.hazards.push_back(p);
  }
  out.foodSpawning = false;
  out.snakes.resize(f.nSnakes);
  for (int i = 0; i < f.nSnakes; ++i) {
    const FastSnake& src = f.snakes[i];
    SnakeState& dst = out.snakes[i];
    const std::string id = (i < static_cast<int>(tpl.snakes.size())) ? tpl.snakes[i].id : "";
    dst.id = id;
    dst.health = src.health;
    dst.alive = src.alive;
    dst.cause = ElimCause::NotEliminated;
    dst.eliminatedBy = "";
    dst.body.clear();
    if (src.alive)
      for (int k = 0; k < src.len; ++k) {
        int c = src.cellAt(k);
        dst.body.push_back(Point{src.cellAt(k) % f.geom.W, c / f.geom.W});
      }
  }
}

void makeTurnFast(FastState& f, const Dir* moves, TurnUndo& u) {
  u.turn = f.turn;
  u.occ = f.occ;
  u.food = f.food;
  u.key = f.key;
  for (int i = 0; i < f.nSnakes; ++i) {
    u.bodyB[i] = f.bodyB[i];
    u.snake[i].health = f.snakes[i].health;
    u.snake[i].len = f.snakes[i].len;
    u.snake[i].headIdx = f.snakes[i].headIdx;
    u.snake[i].alive = f.snakes[i].alive;
    u.seqH[i] = f.snakes[i].seqH;
    if (f.snakes[i].len >= 1)
      u.snake[i].preTailCell = static_cast<uint16_t>(
          f.snakes[i].ring[(f.snakes[i].headIdx + f.snakes[i].len - 1) & kRingMask]);
  }
  const BitGeom& g = f.geom;
  const int n = f.nSnakes;
  int headCell[kMaxFastSnakes];

  // 1. Movement: write the new head into the ring one slot earlier; the
  //    vacated tail drops out of the body unless it is duplicated.
  for (int i = 0; i < n; ++i) {
    FastSnake& s = f.snakes[i];
    if (!s.alive) {
      headCell[i] = -1;
      continue;
    }
    const Dir d = moves[i];
    const int hx = g.xOf(s.headCell()) + (d == Dir::Right) - (d == Dir::Left);
    const int hy = g.yOf(s.headCell()) + (d == Dir::Up) - (d == Dir::Down);
    const int hc = (hx >= 0 && hx < g.W && hy >= 0 && hy < g.H) ? g.idx(hx, hy) : -1;
    headCell[i] = hc;
    if (hc < 0) continue;  // out of bounds: body untouched, dies in phase 5a
    const int L = s.len;   // pre-move length (a move never changes it)
    s.headIdx = (s.headIdx + kMaxRing - 1) & kRingMask;
    s.ring[s.headIdx] = static_cast<uint16_t>(hc);
    f.bodyB[i].set(hc);
    const int poppedCell = s.ring[(s.headIdx + s.len) & kRingMask];
    if (poppedCell != s.tailCell()) f.bodyB[i].reset(poppedCell);
    // sequence hash: prepend the new head, drop the old tail (exponent L-1).
    // The key holds the snake component (seqH ^ health), so a seqH change
    // must be folded into the key as old ^ new.
    const uint64_t seq0 = s.seqH;
    s.seqH = zt.w[i][hc] + zt.beta * (s.seqH - zt.w[i][poppedCell] * zt.betaPow[L - 1]);
    f.key ^= seq0 ^ s.seqH;
  }

  // 2. Starvation tick.
  for (int i = 0; i < n; ++i) {
    if (!f.snakes[i].alive) continue;
    const int h0 = f.snakes[i].health;
    f.snakes[i].health -= 1;
    f.key ^= zt.health[i][hltIdx(h0)] ^ zt.health[i][hltIdx(h0 - 1)];
  }

  // 3. Hazard damage: head on a hazard with no food on the same cell;
  //    an immediate kill also skips feeding (phase 4).
  if (f.hazardDamage > 0) {
    for (int i = 0; i < n; ++i) {
      FastSnake& s = f.snakes[i];
      if (!s.alive || headCell[i] < 0) continue;
      if (f.hazards.test(headCell[i]) && !f.food.test(headCell[i])) {
        const int h0 = s.health;
        s.health -= f.hazardDamage;
        if (s.health < 0) s.health = 0;
        f.key ^= zt.health[i][hltIdx(h0)] ^ zt.health[i][hltIdx(s.health)];
        if (s.health <= 0) {
          s.alive = false;  // eliminate: body stops blocking immediately
          removeSnakeComponent(f, i);
          f.occ &= ~f.bodyB[i];
          f.bodyB[i].clearAll();
        }
      }
    }
  }

  // 4. Feeding: grow by duplicating the tail, reset health, eat the food.
  Bits eaten;
  eaten.clearAll();
  for (int i = 0; i < n; ++i) {
    FastSnake& s = f.snakes[i];
    if (!s.alive || headCell[i] < 0) continue;
    if (f.food.test(headCell[i])) {
      const int tailC = s.tailCell();  // pre-grow tail; becomes duplicated
      s.ring[(s.headIdx + s.len) & kRingMask] = static_cast<uint16_t>(tailC);
      const uint64_t seq0 = s.seqH;
      s.seqH += zt.w[i][tailC] * zt.betaPow[s.len];  // append at exponent len
      f.key ^= seq0 ^ s.seqH;
      ++s.len;
      const int h0 = s.health;
      s.health = 100;
      f.key ^= zt.health[i][hltIdx(h0)] ^ zt.health[i][100];
      eaten.set(headCell[i]);
    }
  }
  f.food &= ~eaten;
  {
    Bits e = eaten;
    for (int wi = 0; wi < kMaxBitWords; ++wi) {
      uint64_t wv = e.w[wi];
      while (wv) {
        f.key ^= zt.foodC[(wi << 6) + __builtin_ctzll(wv)];
        wv &= wv - 1;
      }
    }
  }

  // 5a. Elimination phase one: starvation and out-of-bounds.
  for (int i = 0; i < n; ++i) {
    FastSnake& s = f.snakes[i];
    if (!s.alive) continue;
    if (s.health <= 0 || headCell[i] < 0) {
      s.alive = false;
      removeSnakeComponent(f, i);
      f.occ &= ~f.bodyB[i];
      f.bodyB[i].clearAll();
    }
  }

  // 5b. Phase two: collisions collected among survivors, applied together.
  //     Pass 1 decides self-collisions (a snake that moved onto its own
  //     body has its head cell repeated in its post body, which bitsets
  //     cannot see - everyone must know before checking bodies). Pass 2
  //     collects body collisions and head-to-heads among all survivors.
  bool dieSelf[kMaxFastSnakes] = {false};
  for (int i = 0; i < n; ++i) {
    const FastSnake& s = f.snakes[i];
    if (!s.alive) continue;
    const int hc = headCell[i];
    if (hc < 0) continue;  // already eliminated in 5a
    {
      const Bits& pre = u.bodyB[i];  // pre-turn body bits
      const int preLen = u.snake[i].len;
      const uint16_t preHeadIdx = u.snake[i].headIdx;
      // A single-segment snake that eats duplicates the head cell itself:
      // post body [h, h] means the head is inside body[1..] - self-collision.
      if (preLen == 1 && s.len == 2) {
        dieSelf[i] = true;
        continue;
      }
      if (s.dirty) {
        // exact: scan pre body positions 1..len-2 for the new head
        for (int k = 1; k + 1 < preLen; ++k)
          if (s.ring[(preHeadIdx + k) & kRingMask] == hc) {
            dieSelf[i] = true;
            break;
          }
      } else {
        const int preTail = (preHeadIdx + preLen - 1) & kRingMask;
        // The pre-tail slot may already hold the duplicate-tail write of
        // this turn's feeding (the dup lands exactly there), so the pre-turn
        // tail cell comes from the undo snapshot. Slot preTail-1 is never
        // written by a turn, so the duplicate test can read the live ring.
        const int preTailCell = u.snake[i].preTailCell;
        const bool preDup =
            preLen >= 2 && preTailCell == s.ring[(preTail + kMaxRing - 1) & kRingMask];
        const bool tailSafe = (hc == preTailCell) && !preDup;
        if (pre.test(hc) && !tailSafe) dieSelf[i] = true;
      }
    }
  }

  bool die[kMaxFastSnakes] = {false};
  for (int i = 0; i < n; ++i) {
    const FastSnake& s = f.snakes[i];
    if (!s.alive) continue;
    const int hc = headCell[i];
    if (hc < 0) continue;  // already eliminated in 5a
    if (dieSelf[i]) {
      die[i] = true;
      continue;
    }
    // Body collision with another survivor's post-move body[1..]. For a
    // clean body the head cell appears exactly once (position 0), so a
    // bitset test with a head-cell guard is exact. Bodies dirtied either
    // at conversion or by moving onto themselves this turn can repeat the
    // head cell at a later position, which the bitset cannot see: those
    // take the exact scan.
    bool bodyHit = false;
    for (int j = 0; j < n && !bodyHit; ++j) {
      if (j == i) continue;
      if (!f.snakes[j].alive || headCell[j] < 0) continue;
      const bool jDirty = f.snakes[j].dirty || dieSelf[j];
      if (jDirty) {
        const FastSnake& o = f.snakes[j];
        for (int k = 1; k < o.len; ++k)
          if (o.cellAt(k) == hc) {
            bodyHit = true;
            break;
          }
      } else if (headCell[j] != hc && f.bodyB[j].test(hc)) {
        bodyHit = true;
      }
    }
    if (bodyHit) {
      die[i] = true;
      continue;
    }
    // Head-to-head: same head cell and not strictly longer.
    for (int j = 0; j < n; ++j) {
      if (j == i) continue;
      if (!f.snakes[j].alive || headCell[j] < 0) continue;
      if (headCell[j] == hc && s.len <= f.snakes[j].len) {
        die[i] = true;
        break;
      }
    }
  }
  for (int i = 0; i < n; ++i) {
    if (dieSelf[i]) die[i] = true;
  }
  for (int i = 0; i < n; ++i) {
    if (!die[i]) continue;
    f.snakes[i].alive = false;
    removeSnakeComponent(f, i);
    f.occ &= ~f.bodyB[i];
    f.bodyB[i].clearAll();
  }

  // occ is maintained incrementally; recompute to stay canonical.
  f.occ.clearAll();
  for (int i = 0; i < n; ++i)
    if (f.snakes[i].alive) f.occ |= f.bodyB[i];

  f.key ^= zt.turnC[f.turn & 2047] ^ zt.turnC[(f.turn + 1) & 2047];
  ++f.turn;
}

void unmakeTurnFast(FastState& f, const TurnUndo& u) {
  f.turn = u.turn;
  f.occ = u.occ;
  f.food = u.food;
  f.key = u.key;
  for (int i = 0; i < f.nSnakes; ++i) {
    const int postLen = f.snakes[i].len;  // read before restoring
    f.bodyB[i] = u.bodyB[i];
    f.snakes[i].health = u.snake[i].health;
    f.snakes[i].len = u.snake[i].len;
    f.snakes[i].headIdx = u.snake[i].headIdx;
    f.snakes[i].alive = u.snake[i].alive;
    f.snakes[i].seqH = u.seqH[i];
    // The duplicate-tail write of a feeding turn landed on the pre-turn
    // span's last slot ((headIdx+len-1) of the RESTORED geometry): put the
    // original cell back. Only a feeding snake (len grew by exactly one)
    // wrote there.
    if (u.snake[i].len >= 1 && postLen == u.snake[i].len + 1)
      f.snakes[i].ring[(u.snake[i].headIdx + u.snake[i].len - 1) & kRingMask] =
          u.snake[i].preTailCell;
  }
}

FloodResult floodFill(const BitGeom& g, const Bits& blocked, const Bits& start,
                      const Bits* target) {
  FloodResult r;
  // The start cell is seeded unconditionally (the old BFS gave the head
  // dist 0 even though heads are part of the blocked grid); only the
  // expansion honors `blocked`.
  Bits frontier = start;
  Bits reached = start;
  if (target && !(frontier & *target).none()) r.firstHit = 0;
  for (int layer = 1; !frontier.none(); ++layer) {
    frontier = neighbors(frontier, g) & ~blocked & ~reached;
    reached |= frontier;
    if (target && r.firstHit < 0 && !(frontier & *target).none())
      r.firstHit = layer;
  }
  r.reached = reached;
  return r;
}

void voronoi(const BitGeom& g, const Bits& blocked, const Bits* heads, int n,
             VoronoiResult& out) {
  // Old-semantics twin: each snake's BFS runs independently over the free
  // grid (paths may cross other snakes' cells), then cells are owned by the
  // unique nearest head; equal-distance cells are contested (unclaimed).
  // Layered BFS gives the distances; the merge walks layers ascending.
  std::vector<Bits> layers[kMaxFastSnakes];
  Bits cumulative[kMaxFastSnakes];  // cells reached by snake t at layers < L
  for (int i = 0; i < n; ++i) {
    out.claimed[i].clearAll();
    cumulative[i].clearAll();
    layers[i].push_back(heads[i]);  // layer 0: the head itself
    Bits frontier = heads[i];
    Bits reached = heads[i];
    while (!frontier.none()) {
      frontier = neighbors(frontier, g) & ~blocked & ~reached;
      reached |= frontier;
      layers[i].push_back(frontier);
    }
    cumulative[i] = heads[i];  // layer 0 processed below
  }
  const size_t maxLayers = [&] {
    size_t m = 0;
    for (int i = 0; i < n; ++i) m = std::max(m, layers[i].size());
    return m;
  }();
  Bits othersEarlier[kMaxFastSnakes];  // cells any other snake reached < L
  for (int i = 0; i < n; ++i) {
    othersEarlier[i].clearAll();
    for (int t = 0; t < n; ++t)
      if (t != i) othersEarlier[i] |= layers[t][0];
  }
  for (size_t L = 0; L < maxLayers; ++L) {
    Bits candidates[kMaxFastSnakes];
    Bits anyCandidate;
    anyCandidate.clearAll();
    for (int i = 0; i < n; ++i) {
      candidates[i] = (L < layers[i].size()) ? layers[i][L] : Bits{};
      candidates[i] &= ~othersEarlier[i];  // someone else was strictly nearer
      anyCandidate |= candidates[i];
    }
    if (anyCandidate.none()) {
      // still advance the cumulative sets for this layer
      for (int i = 0; i < n; ++i)
        for (int t = 0; t < n; ++t)
          if (t != i && L < layers[t].size()) othersEarlier[i] |= layers[t][L];
      continue;
    }
    // equal-layer overlaps are contested: compute the full tied set on the
    // pristine candidates first, then drop it from every snake
    Bits tied;
    tied.clearAll();
    for (int i = 0; i < n; ++i)
      for (int t = i + 1; t < n; ++t) tied |= candidates[i] & candidates[t];
    for (int i = 0; i < n; ++i) {
      candidates[i] &= ~tied;
      out.claimed[i] |= candidates[i];
    }
    for (int i = 0; i < n; ++i)
      for (int t = 0; t < n; ++t)
        if (t != i && L < layers[t].size()) othersEarlier[i] |= layers[t][L];
  }
}

// ---------------------- tail-aware flood fill (hobbs port) -----------------

namespace {

// Shared core of tailAwareVoronoi / tailAwareFlood. `order` lists the alive
// snake indices in processing order (longest first; caller fixes ties
// deterministically). Every cycle: release each body's next square into
// `open` (the ring walk below), expand every live frontier once, longest
// first, through cells not yet claimed and enterable this cycle.
void tailAwareCore(const FastState& f, const int* order, int n, int cyclesCap,
                   TailAwareResult& out) {
  for (int i = 0; i < f.nSnakes; ++i) {
    out.entered[i].clearAll();
    out.claimed[i].clearAll();
  }
  out.blockedFinal.clearAll();

  Bits visited;  // global claim mask: heads seeded, entries claim permanently
  Bits open;     // body squares already passed by some tail (enterable)
  Bits frontier[kMaxFastSnakes];
  for (int k = 0; k < n; ++k) {
    const int s = order[k];
    const int head = f.snakes[s].headCell();
    visited.set(head);
    frontier[s] = Bits{};  // zero-init
    frontier[s].set(head);
    out.entered[s].set(head);
  }

  for (int cycle = 1; cycle <= cyclesCap; ++cycle) {
    bool anyAlive = false;
    for (int k = 0; k < n; ++k)
      if (!frontier[order[k]].none()) {
        anyAlive = true;
        break;
      }
    if (!anyAlive) break;

    // Release phase. The square at ring index i = len - cycle opens this
    // cycle iff it is the head-ward-most occurrence of its cell (ring[i] !=
    // ring[i-1]): for a stacked run only the last occurrence carries the
    // cell's true vacate time, which is what gives a double-stacked
    // (just-fed) tail its extra tick.
    for (int k = 0; k < n; ++k) {
      const FastSnake& sn = f.snakes[order[k]];
      const int i = sn.len - cycle;
      if (i < 0) continue;
      if (i == 0 || sn.cellAt(i) != sn.cellAt(i - 1)) open.set(sn.cellAt(i));
    }
    const Bits blocked = f.occ & ~open;

    // Expansion phase, longest first: same-cycle contests are claimed by the
    // earlier (longer) snake via `visited`.
    for (int k = 0; k < n; ++k) {
      const int s = order[k];
      if (frontier[s].none()) continue;
      Bits nxt = neighbors(frontier[s], f.geom) & ~visited & ~blocked;
      visited |= nxt;
      out.entered[s] |= nxt;
      frontier[s] = nxt;
    }
  }

  out.blockedFinal = f.occ & ~open;
  for (int s = 0; s < f.nSnakes; ++s) {
    if (!f.snakes[s].alive) continue;
    // Ownership grid: entered squares plus own body squares nobody entered.
    out.claimed[s] = out.entered[s] | (f.bodyB[s] & ~visited);
  }
}

}  // namespace

void tailAwareVoronoi(const FastState& f, int cyclesCap, TailAwareResult& out) {
  int order[kMaxFastSnakes];
  int n = 0;
  for (int s = 0; s < f.nSnakes; ++s)
    if (f.snakes[s].alive) order[n++] = s;
  // Longest first; stable so equal lengths keep ascending id order (the
  // claim priority must be deterministic for the eval/TT path).
  std::stable_sort(order, order + n, [&](int a, int b) {
    return f.snakes[a].len > f.snakes[b].len;
  });
  tailAwareCore(f, order, n, cyclesCap, out);
}

void tailAwareFlood(const FastState& f, int snakeIdx, int cyclesCap,
                    Bits& enteredOut) {
  TailAwareResult r;
  const int order[1] = {snakeIdx};
  tailAwareCore(f, order, 1, cyclesCap, r);
  enteredOut = r.entered[snakeIdx];
}

}  // namespace bs
