// v2/v4 search: paranoid minimax with alpha-beta pruning and iterative
// deepening, plus the v4a Stockfish-style pruning layer (aspiration
// windows, late-move reductions with re-search, futility pruning, killer
// replies, root ordering by previous value) and the v4b endgame cutoff
// (enemy cycle-room tracking under territory dominance).
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
// v5: a Zobrist-keyed transposition table caches search results (bounds by
// remaining depth) and static leaf evaluations across iterative-deepening
// iterations and game turns, keyed on the full position identity (body
// sequences, healths, food, turn, board shape, viewer).
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
  long nodes = 0;           // search nodes visited (benchmarking)
  long ttHits = 0;          // transposition table hits (probe -> usable entry)
};

struct SearchOptions {
  double budgetMs = 25.0;
  int maxDepth = 16;
  // Strict mode: disable LMR and futility (the heuristic approximations) and
  // the transposition table - a pure, deterministic alpha-beta reference
  // used by the TT equivalence tests.
  bool strict = false;
  bool useTT = true;
};

// Picks a move for snake myId within the time budget.
SearchResult searchV2(const GameState& g, const std::string& myId,
                      const SearchOptions& opts);

// Legacy signature: budget only, everything else default.
SearchResult searchV2(const GameState& g, const std::string& myId, double timeBudgetMs);

// Convenience wrapper returning just the move.
Dir chooseMoveV2(const GameState& g, const std::string& myId, double timeBudgetMs);

}  // namespace bs
