// v1 survival core: rules-correct legal move generation, head-to-head risk
// avoidance, flood-fill enclosure guard, hunger-aware food targeting and a
// tail-chase fallback. Pure function of the request state (thread-safe).
#pragma once

#include "rules.hpp"

#include <climits>
#include <string>
#include <vector>

namespace bs {

struct MoveEval {
  Dir move = Dir::Up;
  bool legal = false;        // in bounds and not on a post-move body cell
  bool h2hRisk = false;      // an equal-or-longer enemy head may claim the cell
  bool hazardCost = false;   // enters a hazard cell without food on it
  int floodSpace = 0;        // free cells reachable from the new head
  int distToFood = INT_MAX;  // BFS distance to the nearest reachable food
  int distToTail = INT_MAX;  // BFS distance to own (vacating) tail cell
  double score = 0.0;
};

// Evaluates all four moves for snake myId (see MoveEval). Snakes that are
// absent or dead get four illegal moves.
std::vector<MoveEval> evaluateMoves(const GameState& g, const std::string& myId);

// Picks the highest-scoring legal move; falls back to "up" when doomed.
Dir chooseMoveV1(const GameState& g, const std::string& myId);

}  // namespace bs
