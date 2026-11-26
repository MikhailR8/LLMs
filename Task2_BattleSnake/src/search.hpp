// v2: paranoid minimax with alpha-beta pruning and iterative deepening.
//
// Model: each tree level is one full game turn. Within a turn, my move is
// chosen first (max) and every enemy's reply is then enumerated (min) - the
// "paranoid" reading of the simultaneous game: enemies get to counter my
// exact move, so the search value is a sound lower bound on my position.
// All moves of a turn are still applied SIMULTANEOUSLY through the rules
// engine (bs::applyTurn), so head-to-head, tail-vacating and two-phase
// eliminations are exact. Future food spawns (random map logic) are excluded
// from the search to keep it deterministic.
//
// Safety: the root only considers moves that pass the v1 legal filter, and
// any failure (exception, exhausted budget with no result) falls back to the
// v1 choice - v2 can never return a move that v1 would reject.
#pragma once

#include "rules.hpp"

#include <string>

namespace bs {

struct SearchResult {
  Dir bestMove = Dir::Up;
  int depthReached = 0;     // deepest fully completed iteration
  double elapsedMs = 0.0;
  double score = 0.0;       // root value of the chosen move
  bool fellBackToV1 = false;
  bool forcedWin = false;   // stopped early on a proven win
};

// Picks a move for snake myId within timeBudgetMs of wall-clock time.
// Thread-safe: no shared mutable state.
SearchResult searchV2(const GameState& g, const std::string& myId, double timeBudgetMs);

// Convenience wrapper returning just the move.
Dir chooseMoveV2(const GameState& g, const std::string& myId, double timeBudgetMs);

}  // namespace bs
