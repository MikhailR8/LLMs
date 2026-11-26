// FastState: the bitboard twin of GameState for the search hot path.
//
// Semantics mirror rules.cpp applyTurn EXACTLY for the search's operating
// point: foodSpawning = false (future food is random and excluded), no
// ruleset variants beyond hazard damage. Equivalence is enforced by a
// differential fuzz test (tests/test_fastboard.cpp) that replays random
// states and moves through both engines and compares the results.
//
// Bodies live in a ring buffer per snake: move = write the new head one
// slot before the current head (the popped tail's slot keeps stale data,
// which is exactly the duplicated-tail semantics: feeding then just grows
// the length, with one write to duplicate the tail cell).
#pragma once

#include "board.hpp"
#include "rules.hpp"

namespace bs {

struct FastSnake {
  uint16_t ring[kMaxRing] = {0};
  uint16_t headIdx = 0;  // ring slot of body[0]
  int len = 0;
  int health = 100;
  bool alive = true;
  // True when the converted body has non-adjacent repeated cells (possible
  // only in adversarial/fuzz inputs - real games only grow by adjacent tail
  // duplicates). Dirty bodies take the exact O(len) self-collision scan;
  // clean bodies (the live invariant) take the bitset fast path.
  bool dirty = false;
  // Rolling polynomial hash of the body sequence: H = sum w[cell(b_i)] *
  // beta^i (head at exponent 0). A move prepends the head and drops the
  // tail in O(1): H := w(hc) + beta * (H - w(t) * beta^(L-1)); feeding
  // appends the duplicated tail: H += w(t') * beta^L. The sequence (not
  // the cell set) must be hashed: the vacating order determines the future.
  uint64_t seqH = 0;

  int cellAt(int i) const { return ring[(headIdx + i) & kRingMask]; }
  int headCell() const { return ring[headIdx]; }
  int tailCell() const { return ring[(headIdx + len - 1) & kRingMask]; }
  bool tailDuplicated() const {
    return len >= 2 && cellAt(len - 1) == cellAt(len - 2);
  }
};

struct FastState {
  BitGeom geom;
  int turn = 0;
  int nSnakes = 0;
  int hazardDamage = 0;
  Bits occ;                          // union of alive snakes' bodies
  Bits food, hazards;
  Bits bodyB[kMaxFastSnakes];        // per-snake body cells (deduplicated)
  FastSnake snakes[kMaxFastSnakes];
  // Zobrist identity of the position: salts (board shape, viewer index,
  // hazard damage) ^ turn ^ food ^ hazards ^ XOR over alive snakes of
  // (seqH ^ health). Maintained incrementally by makeTurnFast; restored by
  // unmakeTurnFast. Sound even for dirty bodies: the full sequence is in
  // the key, so no two different futures can collide on it.
  uint64_t key = 0;

  int aliveCount() const {
    int n = 0;
    for (int i = 0; i < nSnakes; ++i)
      if (snakes[i].alive) ++n;
    return n;
  }
  // Twin of rules.cpp blockedAfterMove: body[0..len-2] per alive snake.
  Bits blockedAfterMoveBits() const;
};

// Undo record: rings never need restoration except the duplicate-tail slot
// (see unmakeTurnFast); everything else is small enough to snapshot.
struct TurnUndo {
  int turn = 0;
  Bits occ, food;
  Bits bodyB[kMaxFastSnakes];
  struct {
    int health, len;
    uint16_t headIdx;
    bool alive;
    uint16_t preTailCell;  // ring slot (headIdx+len-1) value before the turn:
                           // the duplicate-tail write lands exactly there and
                           // unmake must put it back
  } snake[kMaxFastSnakes];
  uint64_t key = 0;
  uint64_t seqH[kMaxFastSnakes];  // rolling body-sequence hashes
};

// False when the game uses a shape the bitboard path does not support
// (board > 512 cells or more than 8 snakes) - caller falls back to v1.
// meIdx salts the hash with the search's viewer: the same position is a
// different game position for a different snake.
bool fastFromGame(const GameState& g, FastState& out, int meIdx = 0);

// Reference re-derivation of the Zobrist key from the state fields
// (independent of the incremental maintenance). Tests use it to verify
// that makeTurnFast keeps f.key exact.
uint64_t computePositionKey(const FastState& f, int meIdx);

// Back-conversion for differential testing (identities only matter for
// debugging; body of dead snakes is not reconstructed).
void fastToGame(const FastState& f, const GameState& tpl, GameState& out);

// One full turn, exact applyTurn order (movement, health, hazards, feeding,
// starvation/out-of-bounds, collisions, no food spawn). moves[i] is used
// for alive snakes only.
void makeTurnFast(FastState& f, const Dir* moves, TurnUndo& undo);
void unmakeTurnFast(FastState& f, const TurnUndo& undo);

// Layered bitboard flood fill from `start` over free (non-blocked) cells.
// Returns the reached set (start included) and, when target != nullptr, the
// first BFS layer whose frontier intersects the target (-1 when never).
struct FloodResult {
  Bits reached;
  int firstHit = -1;
};
FloodResult floodFill(const BitGeom& g, const Bits& blocked, const Bits& start,
                      const Bits* target);

// Simultaneous multi-source layered BFS: each cell is claimed by the snake
// whose head reaches it first; cells reached first by two or more snakes in
// the same layer stay unclaimed (the old per-snake-BFS "contested" twin).
struct VoronoiResult {
  Bits claimed[kMaxFastSnakes];
};
void voronoi(const BitGeom& g, const Bits& blocked, const Bits* heads, int n,
             VoronoiResult& out);

// Tail-aware flood fill: port of hovering-hobbs spread_from_head_tail_aware.
//
// Semantics: a body cell at ring index i (head = 0) is enterable on cycle
// len - i, taking the head-ward-most occurrence for stacked squares (a
// just-fed double-stacked tail needs the extra tick to clear). All alive
// heads spread one ring per cycle; within a cycle snakes are processed
// longest-first, so the longer snake wins same-cycle contests. A frontier
// with no enterable neighbour dies and never retries - the fill models
// paths that move every tick, which is what a snake can actually do.
// Growth is ignored (vacate times optimistic by one tick per food eaten;
// the same approximation the original makes).
//
// blockedAtCycle(c) = occ & ~open, where `open` accumulates the released
// cells - O(1) multiword ops per cycle, no per-cell arrays.
struct TailAwareResult {
  Bits entered[kMaxFastSnakes];   // cells each frontier walked onto (pairwise
                                  // disjoint; the head is seeded)
  Bits claimed[kMaxFastSnakes];   // ownership: entered | (bodyB & ~visited)
                                  // (never-entered own body squares)
  Bits blockedFinal;              // body cells still unenterable at the end
};
void tailAwareVoronoi(const FastState& f, int cyclesCap, TailAwareResult& out);

// Single-source variant: only snakeIdx's head spreads, over all bodies'
// vacate schedules. enteredOut = head + walked cells (own wake included).
void tailAwareFlood(const FastState& f, int snakeIdx, int cyclesCap,
                    Bits& enteredOut);

}  // namespace bs
