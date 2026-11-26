#include "search.hpp"

#include "strategy.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace bs {
namespace {

constexpr double kWinScore = 1000000.0;
constexpr int kMaxSearchDepth = 16;
constexpr int kTimeCheckInterval = 1;  // deadline checked at every node entry
constexpr double kInf = kWinScore * 2.0;

// ---------------------------------------------------------------- eval ---

// Scratch buffers reused across evaluations (sized once per search).
struct EvalContext {
  std::vector<uint8_t> blocked;   // cells that stay occupied after the move
  std::vector<int> dist;          // BFS distance grid
  std::vector<int> minDist;       // Voronoi: nearest head distance
  std::vector<int> minOwner;      // -1 none, -2 contested, else snake index
  std::vector<Point> queue;       // BFS frontier
};

void bfsDistances(const GameState& g, const std::vector<uint8_t>& blocked,
                  const Point& start, std::vector<int>& dist,
                  std::vector<Point>& queue) {
  std::fill(dist.begin(), dist.end(), -1);
  queue.clear();
  const int W = g.width;
  auto idx = [&](const Point& p) { return p.y * W + p.x; };
  dist[idx(start)] = 0;
  queue.push_back(start);
  size_t head = 0;
  while (head < queue.size()) {
    Point cur = queue[head++];
    for (int d = 0; d < kNumDirs; ++d) {
      Point nxt = step(cur, static_cast<Dir>(d));
      if (!g.inBounds(nxt)) continue;
      int i = idx(nxt);
      if (blocked[i] || dist[i] != -1) continue;
      dist[i] = dist[idx(cur)] + 1;
      queue.push_back(nxt);
    }
  }
}

// Leaf evaluation from the perspective of snake meIdx (alive, non-terminal).
// Weighted sum of territory, space, food clock, length, mobility and
// head-to-head pressure. Pure heuristic territory: tactics come from depth.
double evalState(const GameState& g, int meIdx, EvalContext& ctx) {
  const int W = g.width;
  const int cells = W * g.height;
  if (ctx.blocked.size() != static_cast<size_t>(cells)) {
    ctx.blocked.assign(cells, 0);
    ctx.dist.assign(cells, -1);
    ctx.minDist.assign(cells, -1);
    ctx.minOwner.assign(cells, -1);
    ctx.queue.reserve(cells);
  } else {
    std::fill(ctx.blocked.begin(), ctx.blocked.end(), 0);
  }
  for (const Point& p : blockedAfterMove(g)) ctx.blocked[p.y * W + p.x] = 1;

  const SnakeState& me = g.snakes[meIdx];

  // --- Voronoi territory: cells whose nearest head is mine only ---
  int myCells = 0;
  int enemyMaxCells = 0;
  {
    std::fill(ctx.minDist.begin(), ctx.minDist.end(), -1);
    std::fill(ctx.minOwner.begin(), ctx.minOwner.end(), -1);
    for (int s = 0; s < static_cast<int>(g.snakes.size()); ++s) {
      if (!g.snakes[s].alive) continue;
      bfsDistances(g, ctx.blocked, g.snakes[s].head(), ctx.dist, ctx.queue);
      for (int i = 0; i < cells; ++i) {
        const int d = ctx.dist[i];
        if (d < 0) continue;  // unreachable or blocked
        if (ctx.minDist[i] < 0 || d < ctx.minDist[i]) {
          ctx.minDist[i] = d;
          ctx.minOwner[i] = s;
        } else if (d == ctx.minDist[i] && ctx.minOwner[i] != s) {
          ctx.minOwner[i] = -2;  // tie -> contested
        }
      }
    }
    for (int i = 0; i < cells; ++i)
      if (ctx.minOwner[i] == meIdx) ++myCells;
    for (int s = 0; s < static_cast<int>(g.snakes.size()); ++s) {
      if (!g.snakes[s].alive || s == meIdx) continue;
      int eCells = 0;
      for (int i = 0; i < cells; ++i)
        if (ctx.minOwner[i] == s) ++eCells;
      enemyMaxCells = std::max(enemyMaxCells, eCells);
    }
  }

  // --- my reachable space and nearest food (one BFS, both answers) ---
  int space = 0;
  int dFood = -1;  // -1: no reachable food
  bfsDistances(g, ctx.blocked, me.head(), ctx.dist, ctx.queue);
  for (int i = 0; i < cells; ++i)
    if (ctx.dist[i] >= 0) ++space;
  for (const Point& f : g.food) {
    const int d = ctx.dist[f.y * W + f.x];
    if (d >= 0 && (dFood < 0 || d < dFood)) dFood = d;
  }

  const int myLen = me.length();
  int maxEnemyLen = 0;
  int adjacentEnemyBias = 0;
  int freeNeighbors = 0;
  for (int d = 0; d < kNumDirs; ++d) {
    Point p = step(me.head(), static_cast<Dir>(d));
    if (g.inBounds(p) && !ctx.blocked[p.y * W + p.x]) ++freeNeighbors;
  }
  for (int s = 0; s < static_cast<int>(g.snakes.size()); ++s) {
    const SnakeState& sn = g.snakes[s];
    if (!sn.alive || s == meIdx) continue;
    maxEnemyLen = std::max(maxEnemyLen, sn.length());
    const int manhattan =
        std::abs(sn.head().x - me.head().x) + std::abs(sn.head().y - me.head().y);
    if (manhattan == 1) adjacentEnemyBias += (sn.length() >= myLen) ? -45 : +25;
  }

  double score = 0.0;
  score += 6.0 * (myCells - enemyMaxCells);             // territory
  if (space < myLen)
    score -= 40.0 * (myLen - space);                    // enclosure
  else
    score += 1.5 * std::min(space, myLen + 8);          // room to roam
  score += 3.0 * (myLen - maxEnemyLen);                 // length differential
  score += 2.0 * freeNeighbors;                         // mobility
  score += 0.02 * me.health;                            // tie-break comfort
  score += static_cast<double>(adjacentEnemyBias);      // head-to-head pressure
  if (dFood >= 0) {
    if (dFood >= me.health)
      score -= 50.0 * (dFood - me.health);              // starve before arrival
    else
      score += 0.5 * std::min(me.health - dFood, 90);   // slack on the food clock
  } else if (!g.food.empty()) {
    score -= 2.0 * (100 - me.health);                   // food exists but sealed off
  }
  return score;
}

// -------------------------------------------------------------- search ---

struct Child {
  GameState state;
  double prior;   // eval(child), or terminal value, for move ordering
  bool terminal;
  double terminalValue;
};

// Terminal value from my perspective: win > draw > any live position > loss.
double terminalValueFor(const GameState& child, int meIdx) {
  if (!child.snakes[meIdx].alive) {
    if (child.aliveCount() == 0)
      return -kWinScore / 4.0 + 200.0 * child.turn;          // mutual death: draw
    return -kWinScore + 500.0 * child.turn;                  // die as late as possible
  }
  return kWinScore - 500.0 * child.turn;                     // sole survivor
}

class Searcher {
 public:
  Searcher(const GameState& root, const std::string& myId, double budgetMs)
      : root_(root), meIdx_(root.indexOf(myId)), myId_(myId), budgetMs_(budgetMs) {
    start_ = std::chrono::steady_clock::now();
    deadline_ = start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double, std::milli>(budgetMs));
  }

  SearchResult run() {
    SearchResult res;
    if (meIdx_ < 0 || !root_.snakes[meIdx_].alive) {
      res.bestMove = Dir::Up;
      return res;
    }
    // Safe default in case nothing completes: the v1 choice.
    const Dir fallback = chooseMoveV1(root_, myId_);
    res.bestMove = fallback;

    if (rootCandidates().size() == 1) {  // forced: no search needed
      res.bestMove = rootCandidates()[0];
      res.elapsedMs = elapsedMs();
      return res;
    }

    Dir best = fallback;
    for (int depth = 1; depth <= kMaxSearchDepth; ++depth) {
      timedOut_ = false;
      Dir iterBest = Dir::Up;
      double iterValue = 0.0;
      const bool have = rootSearch(depth, best, iterBest, iterValue);
      if (timedOut_) break;  // discard the partial iteration
      if (have) {
        best = iterBest;
        res.score = iterValue;
        res.depthReached = depth;
        res.bestMove = best;
        if (iterValue > kWinScore / 2.0) {
          res.forcedWin = true;
          break;  // proven win; deeper search only refines speed
        }
      }
      if (elapsedMs() > budgetMs_ / 2.0) break;  // next depth won't finish
    }
    if (res.depthReached == 0) {
      res.bestMove = fallback;
      res.fellBackToV1 = true;
    }
    res.elapsedMs = elapsedMs();
    return res;
  }

 private:
  const GameState& root_;
  const int meIdx_;
  const std::string myId_;
  const double budgetMs_;
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point deadline_;
  bool timedOut_ = false;
  long nodes_ = 0;
  EvalContext ctx_;
  std::vector<Dir> rootCands_;

  double elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     start_).count();
  }

  bool checkTime() {
    if ((nodes_ & (kTimeCheckInterval - 1)) == 0 &&
        std::chrono::steady_clock::now() >= deadline_) {
      timedOut_ = true;
      return true;
    }
    return false;
  }

  // Per-node shared data: blocked grid + each snake's candidate moves.
  struct NodeMoves {
    std::vector<uint8_t> blocked;
    std::vector<Dir> mine;
    std::vector<int> enemyIdx;              // alive enemies, board order
    std::vector<std::vector<Dir>> perEnemy;  // candidates per enemy
    std::vector<std::vector<Dir>> replies;   // Cartesian product
  };

  void buildNodeMoves(const GameState& g, NodeMoves& nm) {
    const int W = g.width;
    const size_t cells = static_cast<size_t>(W) * g.height;
    nm.blocked.assign(cells, 0);
    for (const Point& p : blockedAfterMove(g)) nm.blocked[p.y * W + p.x] = 1;

    auto legalDirs = [&](const SnakeState& s) {
      std::vector<Dir> out;
      for (int d = 0; d < kNumDirs; ++d) {
        Point nh = step(s.head(), static_cast<Dir>(d));
        if (g.inBounds(nh) && !nm.blocked[nh.y * W + nh.x])
          out.push_back(static_cast<Dir>(d));
      }
      if (out.empty()) out.push_back(Dir::Up);  // doomed; any move dies
      return out;
    };

    nm.mine = legalDirs(g.snakes[meIdx_]);
    nm.enemyIdx.clear();
    nm.perEnemy.clear();
    for (int s = 0; s < static_cast<int>(g.snakes.size()); ++s) {
      if (!g.snakes[s].alive || s == meIdx_) continue;
      nm.enemyIdx.push_back(s);
      nm.perEnemy.push_back(legalDirs(g.snakes[s]));
    }
    // Cartesian product of enemy moves (empty when no enemies are alive).
    nm.replies.clear();
    nm.replies.push_back({});
    for (const auto& opts : nm.perEnemy) {
      std::vector<std::vector<Dir>> next;
      next.reserve(nm.replies.size() * opts.size());
      for (const auto& prefix : nm.replies)
        for (Dir o : opts) {
          std::vector<Dir> ext = prefix;
          ext.push_back(o);
          next.push_back(std::move(ext));
        }
      nm.replies = std::move(next);
    }
  }

  GameState makeChild(const GameState& g, const NodeMoves& nm, Dir myMove,
                      const std::vector<Dir>& reply) {
    GameState child = g;
    child.foodSpawning = false;  // future food spawns are random: excluded
    std::vector<Dir> moves(child.snakes.size(), Dir::Up);
    moves[meIdx_] = myMove;
    for (size_t k = 0; k < nm.enemyIdx.size(); ++k) moves[nm.enemyIdx[k]] = reply[k];
    std::mt19937 dummy(0);  // unused with food spawning off
    applyTurn(child, moves, dummy);
    return child;
  }

  // Builds one Child (apply + terminal detection + ordering prior).
  void buildChild(const GameState& g, const NodeMoves& nm, Dir myMove,
                  const std::vector<Dir>& reply, Child& c) {
    c.state = makeChild(g, nm, myMove, reply);
    if (c.state.snakes[meIdx_].alive && c.state.aliveCount() > 1) {
      c.terminal = false;
      c.prior = evalState(c.state, meIdx_, ctx_);
    } else {
      c.terminal = true;
      c.terminalValue = terminalValueFor(c.state, meIdx_);
      c.prior = c.terminalValue;
    }
  }

  // One turn of the tree: my max over my moves, enemies' min over joint
  // replies (paranoid: they may counter my exact move). Fails soft.
  double searchTurn(const GameState& g, int depth, double alpha, double beta) {
    ++nodes_;
    if (checkTime()) return 0.0;

    NodeMoves nm;
    buildNodeMoves(g, nm);

    double best = -kInf;
    std::vector<Child> children;
    for (Dir m : nm.mine) {
      children.clear();
      children.reserve(nm.replies.size());
      for (const auto& r : nm.replies) {
        Child c;
        buildChild(g, nm, m, r, c);
        children.push_back(std::move(c));
      }
      // Enemies want my worst: order by ascending prior.
      std::sort(children.begin(), children.end(),
                [](const Child& a, const Child& b) { return a.prior < b.prior; });

      double worst = kInf;
      double innerBeta = beta;
      for (Child& c : children) {
        double v;
        if (c.terminal || depth <= 1) {
          v = c.terminal ? c.terminalValue : c.prior;
        } else {
          v = searchTurn(c.state, depth - 1, alpha, innerBeta);
          if (timedOut_) return 0.0;
        }
        worst = std::min(worst, v);
        if (worst <= alpha) break;  // my move m is refuted
        innerBeta = std::min(innerBeta, worst);
      }
      best = std::max(best, worst);
      if (best >= beta) break;  // cutoff at the max node
      alpha = std::max(alpha, best);
    }
    return best;
  }

  const std::vector<Dir>& rootCandidates() {
    if (rootCands_.empty()) {
      NodeMoves nm;
      buildNodeMoves(root_, nm);
      rootCands_ = std::move(nm.mine);
    }
    return rootCands_;
  }

  // Root: same structure, but tracks the argmax and searches the previous
  // iteration's best move first. Returns false when nothing was evaluated.
  bool rootSearch(int depth, Dir prevBest, Dir& outBest, double& outValue) {
    NodeMoves nm;
    buildNodeMoves(root_, nm);

    std::vector<Dir> ordered;
    for (Dir d : nm.mine)
      if (d == prevBest) ordered.push_back(d);
    for (Dir d : nm.mine)
      if (d != prevBest) ordered.push_back(d);

    double alpha = -kInf;
    const double beta = kInf;
    bool have = false;
    std::vector<Child> children;
    for (Dir m : ordered) {
      children.clear();
      children.reserve(nm.replies.size());
      for (const auto& r : nm.replies) {
        Child c;
        buildChild(root_, nm, m, r, c);
        children.push_back(std::move(c));
      }
      std::sort(children.begin(), children.end(),
                [](const Child& a, const Child& b) { return a.prior < b.prior; });

      double worst = kInf;
      for (Child& c : children) {
        double v;
        if (c.terminal || depth <= 1) {
          v = c.terminal ? c.terminalValue : c.prior;
        } else {
          v = searchTurn(c.state, depth - 1, alpha, beta);
          if (timedOut_) return have;  // run() discards partial iterations
        }
        worst = std::min(worst, v);
      }
      if (!have || worst > alpha) {
        alpha = worst;
        outBest = m;
        outValue = worst;
        have = true;
      }
    }
    return have;
  }
};

}  // namespace

SearchResult searchV2(const GameState& g, const std::string& myId, double timeBudgetMs) {
  Searcher s(g, myId, timeBudgetMs);
  return s.run();
}

Dir chooseMoveV2(const GameState& g, const std::string& myId, double timeBudgetMs) {
  return searchV2(g, myId, timeBudgetMs).bestMove;
}

}  // namespace bs
