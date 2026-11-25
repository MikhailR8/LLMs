#include "strategy.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>

namespace bs {
namespace {

// Occupancy grid over the cells that stay blocked after this turn's moves
// (every alive snake's body minus its vacating tail).
struct Grid {
  const GameState& g;
  std::vector<uint8_t> blocked;

  explicit Grid(const GameState& g_)
      : g(g_), blocked(static_cast<size_t>(g_.width) * g_.height, 0) {
    for (const Point& p : blockedAfterMove(g)) blocked[idx(p)] = 1;
  }

  int idx(const Point& p) const { return p.y * g.width + p.x; }
  bool freeAt(const Point& p) const {
    return g.inBounds(p) && !blocked[static_cast<size_t>(idx(p))];
  }
};

// BFS from start over free cells; returns the number of steps to the nearest
// target (0 when start is a target), INT_MAX when no target is reachable.
// Food always sits on free cells, and the tail target is only passed in when
// its cell is free.
int bfsDistance(const GameState& g, const Grid& grid, const Point& start,
                const std::vector<Point>& targets) {
  if (targets.empty()) return INT_MAX;
  std::vector<int> dist(static_cast<size_t>(g.width) * g.height, -1);
  for (const Point& t : targets)
    if (t == start) return 0;

  std::deque<Point> q;
  dist[static_cast<size_t>(grid.idx(start))] = 0;
  q.push_back(start);
  while (!q.empty()) {
    Point cur = q.front();
    q.pop_front();
    for (int d = 0; d < kNumDirs; ++d) {
      Point nxt = step(cur, static_cast<Dir>(d));
      if (!grid.freeAt(nxt)) continue;
      if (dist[static_cast<size_t>(grid.idx(nxt))] != -1) continue;
      dist[static_cast<size_t>(grid.idx(nxt))] =
          dist[static_cast<size_t>(grid.idx(cur))] + 1;
      for (const Point& t : targets)
        if (t == nxt) return dist[static_cast<size_t>(grid.idx(nxt))];
      q.push_back(nxt);
    }
  }
  return INT_MAX;
}

// Number of free cells reachable from start (start included). When
// extraBlocked is given, those cells count as walls too: v1 uses this to
// measure space pessimistically, treating cells an enemy head can enter
// next turn as already sealed.
int floodFill(const GameState& g, const Grid& grid, const Point& start,
              const std::vector<uint8_t>* extraBlocked = nullptr) {
  std::vector<uint8_t> seen(static_cast<size_t>(g.width) * g.height, 0);
  std::deque<Point> q;
  seen[static_cast<size_t>(grid.idx(start))] = 1;
  q.push_back(start);
  int count = 0;
  while (!q.empty()) {
    Point cur = q.front();
    q.pop_front();
    ++count;
    for (int d = 0; d < kNumDirs; ++d) {
      Point nxt = step(cur, static_cast<Dir>(d));
      if (!grid.freeAt(nxt)) continue;
      if (extraBlocked && (*extraBlocked)[static_cast<size_t>(grid.idx(nxt))])
        continue;
      if (seen[static_cast<size_t>(grid.idx(nxt))]) continue;
      seen[static_cast<size_t>(grid.idx(nxt))] = 1;
      q.push_back(nxt);
    }
  }
  return count;
}

// True when an equal-or-longer enemy head is adjacent to the cell, i.e. that
// enemy could move onto the cell this turn and win or tie the head-to-head.
bool h2hRiskAt(const GameState& g, const std::string& myId, int myLen,
               const Point& cell) {
  for (const auto& e : g.snakes) {
    if (!e.alive || e.id == myId) continue;
    if (e.body.empty()) continue;
    int manhattan = std::abs(e.head().x - cell.x) + std::abs(e.head().y - cell.y);
    if (manhattan == 1 && e.length() >= myLen) return true;
  }
  return false;
}

}  // namespace

std::vector<MoveEval> evaluateMoves(const GameState& g, const std::string& myId) {
  std::vector<MoveEval> out;
  out.reserve(4);

  const int meIdx = g.indexOf(myId);
  if (meIdx < 0 || !g.snakes[meIdx].alive) {
    for (int d = 0; d < kNumDirs; ++d) {
      MoveEval ev;
      ev.move = static_cast<Dir>(d);
      ev.score = -1e9;
      out.push_back(ev);
    }
    return out;
  }

  const SnakeState& me = g.snakes[meIdx];
  const int myLen = me.length();
  Grid grid(g);

  // Tail anchor: only useful when the tail cell actually vacates.
  std::vector<Point> tailTargets;
  if (grid.freeAt(me.tail())) tailTargets.push_back(me.tail());

  // Pessimistic space walls: any cell an enemy head can enter next turn can
  // seal the region we are measuring, so it does not count as escape room.
  std::vector<uint8_t> enemyAdj(static_cast<size_t>(g.width) * g.height, 0);
  for (const auto& e : g.snakes) {
    if (!e.alive || e.id == myId || e.body.empty()) continue;
    for (int d = 0; d < kNumDirs; ++d) {
      Point p = step(e.head(), static_cast<Dir>(d));
      if (g.inBounds(p)) enemyAdj[static_cast<size_t>(grid.idx(p))] = 1;
    }
  }

  // Food urgency from the current head: how close are we to starving?
  const int dFoodNow = bfsDistance(g, grid, me.head(), g.food);
  bool urgent = false;
  if (!g.food.empty()) {
    if (dFoodNow == INT_MAX)
      urgent = me.health < 60;  // nothing reachable: keep space priority
    else
      urgent = me.health < 40 || (me.health - dFoodNow) < 25;
  }

  for (int d = 0; d < kNumDirs; ++d) {
    MoveEval ev;
    ev.move = static_cast<Dir>(d);
    const Point nh = step(me.head(), static_cast<Dir>(d));

    ev.legal = grid.freeAt(nh);
    if (!ev.legal) {
      ev.score = -1e9;
      out.push_back(ev);
      continue;
    }

    ev.h2hRisk = h2hRiskAt(g, myId, myLen, nh);
    ev.hazardCost = g.isHazardAt(nh) && !g.isFoodAt(nh);
    ev.floodSpace = floodFill(g, grid, nh, &enemyAdj);
    ev.distToFood = bfsDistance(g, grid, nh, g.food);
    ev.distToTail =
        tailTargets.empty() ? INT_MAX : bfsDistance(g, grid, nh, tailTargets);

    // --- scoring: survival first ---
    double score = 0.0;
    const int spaceCap = myLen + 12;  // beyond this, extra space adds little
    score += 2.0 * std::min(ev.floodSpace, spaceCap);
    if (ev.floodSpace < myLen)
      score -= 25.0 * (myLen - ev.floodSpace);  // enclosure warning

    if (!g.food.empty()) {
      if (urgent) {
        score -= 8.0 * (ev.distToFood == INT_MAX ? 60 : std::min(ev.distToFood, 60));
        if (ev.distToFood == INT_MAX) score -= 400.0;       // food unreachable
        else if (ev.distToFood > me.health) score -= 200.0; // cannot arrive in time
      } else {
        score -= 0.5 * std::min(ev.distToFood, 60);  // mild food bias otherwise
      }
    }

    if (ev.distToTail != INT_MAX)
      score -= 0.15 * std::min(ev.distToTail, 60);  // weak tail anchor

    if (ev.h2hRisk) score -= 120.0;   // avoid losing/tied head-to-head cells
    if (ev.hazardCost) score -= 80.0; // royale: hazard cells cost 14 hp/turn

    ev.score = score;
    out.push_back(ev);
  }
  return out;
}

Dir chooseMoveV1(const GameState& g, const std::string& myId) {
  const std::vector<MoveEval> evals = evaluateMoves(g, myId);
  bool found = false;
  Dir best = Dir::Up;
  double bestScore = 0.0;
  for (const MoveEval& e : evals) {
    if (!e.legal) continue;
    if (!found || e.score > bestScore) {
      found = true;
      best = e.move;
      bestScore = e.score;
    }
  }
  return found ? best : Dir::Up;  // doomed: still answer with a valid move
}

}  // namespace bs
