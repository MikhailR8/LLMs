// v3/v4/v5 search: paranoid minimax with alpha-beta pruning and iterative
// deepening over the bitboard FastState (make/unmake instead of state
// copies), plus the v4a Stockfish-style pruning layer (aspiration windows,
// late-move reductions with re-search, futility pruning, killer replies,
// root ordering by previous value), the v4b endgame cutoff, and the v5
// Zobrist-keyed transposition table (search bounds by remaining depth +
// a static-eval cache shared across iterations and game turns).
//
// The tree model and evaluation are IDENTICAL to the pre-bitboard v2:
// each level is one full turn (my max move, then the enemies' joint min
// reply, applied simultaneously through the rules); the leaf eval is the
// same weighted sum computed with bitboard BFS. Equivalence of the turn
// engine to rules.cpp applyTurn is proven by tests/test_fastboard.cpp;
// the TT's value-preservation is proven by tests/test_tt.cpp (strict
// alpha-beta with and without the TT must agree).
#include "search.hpp"

#include "fastboard.hpp"
#include "strategy.hpp"
#include "tt.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace bs {
namespace {

constexpr double kWinScore = 1000000.0;
constexpr int kMaxSearchDepth = 16;
constexpr int kTimeCheckInterval = 1;  // deadline checked at every node entry
constexpr double kInf = kWinScore * 2.0;
constexpr double kAspiration = 150.0;  // v4a: aspiration half-window (eval units)
constexpr double kFutility = 150.0;    // v4a: futility margin at the horizon
constexpr int kTaCycles = 12;          // tail-aware fill budget (the original's
                                       // tuned standard-board value)

// ---------------------------------------------------------------- eval ---

// Leaf evaluation from the perspective of snake meIdx (alive, non-terminal).
// Same weighted sum as the pre-bitboard v2; every metric comes from
// bitboard BFS (Voronoi via per-snake layered flood + ascending merge).
double evalFast(const FastState& f, int meIdx) {
  const BitGeom& g = f.geom;
  const FastSnake& me = f.snakes[meIdx];
  const Bits blocked = f.blockedAfterMoveBits();

  // --- tail-aware territory (hobbs-port fill): cells whose claimant is me,
  // counted through vacating tails; same-cycle contests go to the longer
  // snake. Cap 12 cycles is the original's tuned budget; walls are the body
  // cells that never opened within it. ---
  TailAwareResult ta;
  tailAwareVoronoi(f, kTaCycles, ta);
  const Bits walls = ta.blockedFinal;
  const int myCells = (ta.claimed[meIdx] & ~walls).popcount();
  int enemyMaxCells = 0;
  Bits enemyClaimed;
  for (int s = 0; s < f.nSnakes; ++s) {
    if (s == meIdx || !f.snakes[s].alive) continue;
    enemyMaxCells = std::max(enemyMaxCells, (ta.claimed[s] & ~walls).popcount());
    enemyClaimed |= ta.claimed[s];
  }

  // --- my reachable space and tail reachability: single-source tail-aware
  // fill with an unbounded-equivalent cap (same cost profile as the static
  // fill it replaces; the fill ends early when the frontier dies). Space now
  // includes vacated body squares (following the wake), which is exactly the
  // endgame survival question the static fill got wrong. ---
  Bits myEntered;
  tailAwareFlood(f, meIdx, g.cells, myEntered);
  const int space = myEntered.popcount();
  const bool tailReachable = myEntered.test(me.tailCell());

  // --- nearest food: the one deliberately static fill (bodies blocked). The
  // food clock must stay pessimistic - a path that only exists through a
  // vacating tail is not food to be counted on (the original reaches the
  // same choice with a body-blocked Dijkstra). ---
  Bits myHead;
  myHead.set(me.headCell());
  const FloodResult fr = floodFill(g, blocked, myHead, &f.food);
  const int dFood = fr.firstHit;  // -1: no reachable food

  // --- v4b (duel endgame cutoff): under clear territory dominance, track
  // the enemy's cycle room - space below its length means it is dying. This
  // is the eval form of "sever the enemy's region": positions that shrink
  // enemy space or cross its death threshold get targeted directly. The
  // enemy's space comes from the same tail-aware fill now (no extra BFS). ---
  int enemySpace = -1;  // only computed under dominance
  int enemyLen = -1;
  if (f.aliveCount() == 2 && myCells > enemyMaxCells + 8) {
    for (int s = 0; s < f.nSnakes; ++s) {
      if (s == meIdx || !f.snakes[s].alive) continue;
      const int es = (ta.claimed[s] & ~walls).popcount();
      if (es > enemySpace) {
        enemySpace = es;
        enemyLen = f.snakes[s].len;
      }
    }
  }

  const int myLen = me.len;
  int maxEnemyLen = 0;
  int adjacentEnemyBias = 0;
  const Bits nb = neighbors(myHead, g) & ~blocked;
  const int freeNeighbors = nb.popcount();
  for (int s = 0; s < f.nSnakes; ++s) {
    if (s == meIdx || !f.snakes[s].alive) continue;
    const FastSnake& sn = f.snakes[s];
    maxEnemyLen = std::max(maxEnemyLen, sn.len);
    if (nb.test(sn.headCell()))
      adjacentEnemyBias += (sn.len >= myLen) ? -45 : +25;  // manhattan == 1
  }

  double score = 0.0;
  score += 8.0 * (myCells - enemyMaxCells);             // territory
  // Cycle room: survival in a sealed region means space > length (with a
  // growth margin). The slope must dominate the food/health rewards, or the
  // snake happily eats itself into a corner ("food trap" endgame).
  const int cycleRoom = space - (myLen + 4);
  if (cycleRoom < 0)
    score -= 100.0 * (-cycleRoom);                      // enclosure (early + steep)
  else
    score += 1.5 * std::min(space, myLen + 8);          // room to roam
  score += 3.0 * (myLen - maxEnemyLen);                 // length differential
  score += 2.0 * freeNeighbors;                         // mobility
  score += 0.02 * me.health;                            // tie-break comfort
  score += static_cast<double>(adjacentEnemyBias);      // head-to-head pressure
  // Endgame cycle: in a sealed region survival means following your own
  // tail; a reachable tail is a strong live signal, an unreachable one a
  // slow death (same BFS as space/food, so this is free).
  score += tailReachable ? 20.0 : -30.0;
  // v4b: enemy cycle room under dominance - the cutoff target. When the
  // enemy's space drops below its length it is dead barring luck, so such
  // positions get a large direct bonus; otherwise keep squeezing territory
  // at double slope.
  if (enemySpace >= 0) {
    if (enemySpace < enemyLen + 4)
      score += 150.0 + 20.0 * (enemyLen + 4 - enemySpace);
    else
      score += 2.0 * (myCells - enemyMaxCells - 8);
  }
  // Food control: food inside my exclusive territory is mine eventually;
  // food in enemy territory feeds the opposition. Tail-aware claims make
  // food behind a vacating tail mine as well (the wake is enterable).
  score += 10.0 * (f.food & ta.claimed[meIdx] & ~walls).popcount();
  score -= 6.0 * (f.food & enemyClaimed & ~walls).popcount();
  if (dFood >= 0) {
    if (dFood >= me.health) {
      score -= 50.0 * (dFood - me.health);              // starve before arrival
    } else {
      // Urgency grows as health drains: slack on the food clock counts
      // harder when the clock is low.
      const double w = 0.5 + std::max(0, 50 - me.health) * 0.03;
      score += w * std::min(me.health - dFood, 90);     // slack on the food clock
    }
  } else if (!f.food.none()) {
    score -= 5.0 * (100 - me.health);                   // food exists but sealed off
  }
  return score;
}

double terminalValueFast(const FastState& f, int meIdx) {
  if (!f.snakes[meIdx].alive) {
    if (f.aliveCount() == 0)
      return -kWinScore / 4.0 + 200.0 * f.turn;          // mutual death: draw
    return -kWinScore + 500.0 * f.turn;                  // die as late as possible
  }
  return kWinScore - 500.0 * f.turn;                     // sole survivor
}

// -------------------------------------------------------------- search ---

// One candidate line of the tree: my move + the enemies' joint reply.
// The child state itself is not stored - it is replayed through
// makeTurnFast/unmakeTurnFast when needed.
struct ChildEntry {
  Dir m;
  std::vector<Dir> reply;    // enemy joint reply (killer tracking)
  double prior;              // eval(child), or terminal value, for ordering
  bool terminal;
  double termVal;
};

class Searcher {
 public:
  Searcher(const GameState& root, const std::string& myId, const SearchOptions& opts)
      : rootGs_(root),
        meIdx_(root.indexOf(myId)),
        myId_(myId),
        budgetMs_(opts.budgetMs),
        maxDepth_(std::min(opts.maxDepth, kMaxSearchDepth)),
        strict_(opts.strict),
        useTT_(opts.useTT) {
    start_ = std::chrono::steady_clock::now();
    deadline_ = start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double, std::milli>(opts.budgetMs));
    killers_.assign(kMaxSearchDepth + 2, {});
    ok_ = fastFromGame(root, st_, meIdx_ < 0 ? 0 : meIdx_);
    // Strict mode runs pure alpha-beta: it must not read entries written by
    // the approximate (LMR/futility) search nor feed its exact values into
    // the shared table, so it gets a private table instead of the global.
    if (strict_) {
      localTT_ = std::make_unique<TranspositionTable>();
      tt_ = localTT_.get();
    } else {
      tt_ = &globalTT();
    }
  }

  SearchResult run() {
    SearchResult res;
    if (meIdx_ < 0 || !rootGs_.snakes[meIdx_].alive || !ok_) {
      res.bestMove = chooseMoveV1(rootGs_, myId_);
      res.fellBackToV1 = true;
      res.elapsedMs = elapsedMs();
      return res;
    }
    // Safe default in case nothing completes: the v1 choice.
    const Dir fallback = chooseMoveV1(rootGs_, myId_);
    res.bestMove = fallback;

    if (rootCandidates().size() == 1) {  // forced: no search needed
      res.bestMove = rootCandidates()[0];
      res.elapsedMs = elapsedMs();
      return res;
    }

    Dir best = fallback;
    double lastValue = 0.0;
    bool lastValid = false;
    for (int depth = 1; depth <= maxDepth_; ++depth) {
      timedOut_ = false;
      Dir iterBest = Dir::Up;
      double iterValue = 0.0;
      // v4a: aspiration window - search around the previous value instead
      // of (-inf, +inf); a fail low/high triggers one full-window re-search.
      double a = -kInf, b = kInf;
      if (depth >= 3 && lastValid) {
        a = lastValue - kAspiration;
        b = lastValue + kAspiration;
      }
      bool have = rootSearch(depth, best, a, b, iterBest, iterValue);
      if (!timedOut_ && depth >= 3 && lastValid &&
          (!have || iterValue <= a || iterValue >= b)) {
        have = rootSearch(depth, best, -kInf, kInf, iterBest, iterValue);
      }
      if (timedOut_) break;  // discard the partial iteration
      if (have) {
        best = iterBest;
        res.score = iterValue;
        res.depthReached = depth;
        res.bestMove = best;
        lastValue = iterValue;
        lastValid = true;
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
    res.nodes = nodes_;
    res.ttHits = ttHits_;
    return res;
  }

 private:
  const GameState& rootGs_;
  FastState st_;
  bool ok_ = false;
  const int meIdx_;
  const std::string myId_;
  const double budgetMs_;
  const int maxDepth_;
  const bool strict_;
  const bool useTT_;
  TranspositionTable* tt_ = nullptr;                  // global, or local when strict
  std::unique_ptr<TranspositionTable> localTT_;       // strict-mode private table
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point deadline_;
  bool timedOut_ = false;
  long nodes_ = 0;
  long ttHits_ = 0;
  std::vector<Dir> rootCands_;
  std::vector<std::vector<Dir>> killers_;  // v4a: killer reply per depth
  std::map<Dir, double> rootValueMap_;     // v4a: last iteration's root values

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

  // Per-node data: my candidates and the enemies' joint replies.
  struct NodeMoves {
    std::vector<Dir> mine;
    std::vector<int> enemyIdx;              // alive enemies, board order
    std::vector<std::vector<Dir>> replies;  // Cartesian product
  };

  void buildNodeMoves(NodeMoves& nm) {
    const BitGeom& g = st_.geom;
    const Bits blocked = st_.blockedAfterMoveBits();
    auto legalDirs = [&](const FastSnake& s) {
      std::vector<Dir> out;
      const int hc = s.headCell();
      const int hx = g.xOf(hc), hy = g.yOf(hc);
      for (int d = 0; d < kNumDirs; ++d) {
        const int nx = hx + (d == (int)Dir::Right) - (d == (int)Dir::Left);
        const int ny = hy + (d == (int)Dir::Up) - (d == (int)Dir::Down);
        if (nx >= 0 && nx < g.W && ny >= 0 && ny < g.H && !blocked.test(g.idx(nx, ny)))
          out.push_back(static_cast<Dir>(d));
      }
      if (out.empty()) out.push_back(Dir::Up);  // doomed; any move dies
      return out;
    };
    nm.mine = legalDirs(st_.snakes[meIdx_]);
    nm.enemyIdx.clear();
    std::vector<std::vector<Dir>> perEnemy;
    for (int s = 0; s < st_.nSnakes; ++s) {
      if (!st_.snakes[s].alive || s == meIdx_) continue;
      nm.enemyIdx.push_back(s);
      perEnemy.push_back(legalDirs(st_.snakes[s]));
    }
    nm.replies.clear();
    nm.replies.push_back({});
    for (const auto& opts : perEnemy) {
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

  // Applies one (my move, joint reply) turn to st_ (caller unmakes).
  TurnUndo applyLine(Dir myMove, const NodeMoves& nm, const std::vector<Dir>& reply) {
    Dir moves[kMaxFastSnakes] = {Dir::Up};
    moves[meIdx_] = myMove;
    for (size_t k = 0; k < nm.enemyIdx.size(); ++k) moves[nm.enemyIdx[k]] = reply[k];
    TurnUndo u;
    makeTurnFast(st_, moves, u);
    return u;
  }

  // Phase 1 of a max node: evaluate every (move, reply) child once to get
  // ordering priors; the states are unmade again, so the caller is left at
  // the node's own position. Static evals are position-intrinsic, so they
  // are cached in the TT (flag Eval): iterative deepening re-expands this
  // node every iteration and next turn's search passes through many of the
  // same positions again - the eval cache is where the TT pays off most.
  void collectChildren(const NodeMoves& nm, Dir myMove, std::vector<ChildEntry>& out) {
    out.clear();
    out.reserve(nm.replies.size());
    for (const auto& r : nm.replies) {
      TurnUndo u = applyLine(myMove, nm, r);
      ChildEntry e;
      e.m = myMove;
      e.reply = r;
      if (st_.snakes[meIdx_].alive && st_.aliveCount() > 1) {
        e.terminal = false;
        bool cached = false;
        if (useTT_) {
          if (const TTEntry* te = tt_->probe(st_.key)) {
            if (te->flag == static_cast<uint8_t>(TTFlag::Eval)) {
              e.prior = te->value;
              cached = true;
              ++ttHits_;
            }
          }
        }
        if (!cached) {
          e.prior = evalFast(st_, meIdx_);
          if (useTT_) tt_->store(st_.key, e.prior, 0, TTFlag::Eval, 0);
        }
      } else {
        e.terminal = true;
        e.termVal = terminalValueFast(st_, meIdx_);
        e.prior = e.termVal;
      }
      unmakeTurnFast(st_, u);
      out.push_back(std::move(e));
    }
    // Enemies want my worst: order by ascending prior.
    std::sort(out.begin(), out.end(), [](const ChildEntry& a, const ChildEntry& b) {
      return a.prior < b.prior;
    });
  }

  // One turn of the tree: my max over my moves, enemies' min over joint
  // replies (paranoid: they may counter my exact move). Fails soft.
  //
  // v4a prunes the recursion Stockfish-style: late children get a reduced
  // depth (LMR) with a full-depth re-search when they would matter, plainly
  // safe children near the horizon are not searched at all (futility), and
  // the reply that refuted a sibling is tried first here (killer reply).
  //
  // v5: transposition table. Probe at entry - a stored bound with
  // sufficient remaining depth can cut off the whole node; the stored best
  // move is searched first. On exit the (value, bound type, depth, move)
  // tuple is stored, so later iterations and later game turns (whose
  // subtrees are this search's subtrees shifted one ply) re-use it.
  //
  // `approx` propagates the futility taint: a value that passed through a
  // futility shortcut is a heuristic, not a bound, and must not be written
  // to the TT (reading is always sound). LMR cutoffs are stored (standard
  // engine practice - the same approximation already shaped the pre-TT
  // search values).
  double searchTurn(int depth, double alpha, double beta, bool& approx) {
    approx = false;
    ++nodes_;
    if (checkTime()) return 0.0;

    Dir ttMove = Dir::Up;
    if (useTT_) {
      if (const TTEntry* te = tt_->probe(st_.key)) {
        ++ttHits_;
        if (te->flag != static_cast<uint8_t>(TTFlag::Eval) && te->depth >= depth) {
          if (te->flag == static_cast<uint8_t>(TTFlag::Exact)) return te->value;
          if (te->flag == static_cast<uint8_t>(TTFlag::Lower)) {
            if (te->value >= beta) return te->value;
            alpha = std::max(alpha, te->value);
          } else {  // Upper
            if (te->value <= alpha) return te->value;
            beta = std::min(beta, te->value);
          }
        }
        if (te->flag != static_cast<uint8_t>(TTFlag::Eval)) {
          ttMove = static_cast<Dir>(te->move);
        }
      }
    }

    NodeMoves nm;
    buildNodeMoves(nm);
    // TT move first (best move from a previous visit of this position).
    for (size_t i = 1; i < nm.mine.size(); ++i) {
      if (nm.mine[i] == ttMove) {
        std::swap(nm.mine[i], nm.mine[0]);
        break;
      }
    }

    double best = -kInf;
    bool bestApprox = false;
    Dir bestMove = nm.mine.empty() ? Dir::Up : nm.mine[0];
    std::vector<ChildEntry> children;
    for (Dir m : nm.mine) {
      collectChildren(nm, m, children);
      // Killer reply: the joint reply that refuted a sibling move at this
      // depth is a likely refutation here too - search it first.
      if (!killers_[depth].empty()) {
        for (size_t i = 1; i < children.size(); ++i) {
          if (children[i].reply == killers_[depth]) {
            std::swap(children[i], children[0]);
            break;
          }
        }
      }

      double worst = kInf;
      bool worstApprox = false;
      double innerBeta = beta;
      size_t searched = 0;
      for (ChildEntry& c : children) {
        double v;
        bool childApprox = false;
        if (c.terminal || depth <= 1) {
          v = c.terminal ? c.termVal : c.prior;
        } else if (!strict_ && searched >= 2 && depth == 2 &&
                   c.prior >= innerBeta + kFutility) {
          v = c.prior;  // futility: cannot become the minimum, trust the prior
          childApprox = true;
        } else if (!strict_ && searched >= 2 && depth >= 3) {
          TurnUndo u = applyLine(c.m, nm, c.reply);
          v = searchTurn(depth - 2, alpha, innerBeta, childApprox);  // LMR
          if (!timedOut_ && v < innerBeta) {  // matters: verify full depth
            v = searchTurn(depth - 1, alpha, innerBeta, childApprox);
          } else {
            // Unverified reduced cutoff: the value is heuristic, not a
            // bound. The pre-TT search recomputed it fresh every turn; a
            // persistent TT must not amplify the approximation across
            // iterations and turns, so it taints (read is always sound).
            childApprox = true;
          }
          unmakeTurnFast(st_, u);
          if (timedOut_) return 0.0;
        } else {
          TurnUndo u = applyLine(c.m, nm, c.reply);
          v = searchTurn(depth - 1, alpha, innerBeta, childApprox);
          unmakeTurnFast(st_, u);
          if (timedOut_) return 0.0;
        }
        ++searched;
        if (v < worst || (v == worst && worstApprox && !childApprox)) {
          worst = v;
          worstApprox = childApprox;
        }
        if (worst <= alpha) {
          killers_[depth] = c.reply;  // remember the refutation
          break;  // my move m is refuted
        }
        innerBeta = std::min(innerBeta, worst);
      }
      const bool improved = worst > best || (worst == best && bestApprox && !worstApprox);
      if (improved) {
        best = worst;
        bestApprox = worstApprox;
        bestMove = m;
      }
      if (best >= beta) break;  // cutoff at the max node
      alpha = std::max(alpha, best);
    }

    // Store the result unless it passed through a futility approximation
    // or the search was cut short by the deadline.
    if (useTT_ && !timedOut_ && !bestApprox && depth >= 1) {
      TTFlag flag = TTFlag::Exact;
      if (best >= beta)
        flag = TTFlag::Lower;  // fail high: value is a lower bound
      else if (best <= alpha)
        flag = TTFlag::Upper;  // fail low: value is an upper bound
      tt_->store(st_.key, best, static_cast<uint8_t>(depth), flag,
                 static_cast<uint8_t>(bestMove));
    }
    return best;
  }

  const std::vector<Dir>& rootCandidates() {
    if (rootCands_.empty()) {
      NodeMoves nm;
      buildNodeMoves(nm);
      rootCands_ = std::move(nm.mine);
    }
    return rootCands_;
  }

  // Root: same structure, but searches the previous iteration's best move
  // first, orders the rest by their previous-iteration value, and records
  // every root value for the next iteration's ordering. Returns false when
  // nothing was evaluated.
  bool rootSearch(int depth, Dir prevBest, double alphaInit, double betaInit,
                  Dir& outBest, double& outValue) {
    NodeMoves nm;
    buildNodeMoves(nm);

    std::vector<Dir> ordered;
    for (Dir d : nm.mine)
      if (d == prevBest) ordered.push_back(d);
    {
      std::vector<std::pair<double, Dir>> rest;
      for (Dir d : nm.mine) {
        if (d == prevBest) continue;
        auto it = rootValueMap_.find(d);
        rest.push_back({it == rootValueMap_.end() ? -kInf : it->second, d});
      }
      std::sort(rest.begin(), rest.end(),
                [](const std::pair<double, Dir>& a, const std::pair<double, Dir>& b) {
                  return a.first > b.first;
                });
      for (const auto& p : rest) ordered.push_back(p.second);
    }

    double alpha = alphaInit;
    const double beta = betaInit;
    bool have = false;
    std::vector<ChildEntry> children;
    for (Dir m : ordered) {
      collectChildren(nm, m, children);

      double worst = kInf;
      for (ChildEntry& c : children) {
        double v;
        if (c.terminal || depth <= 1) {
          v = c.terminal ? c.termVal : c.prior;
        } else {
          bool childApprox = false;
          TurnUndo u = applyLine(c.m, nm, c.reply);
          v = searchTurn(depth - 1, alpha, beta, childApprox);
          unmakeTurnFast(st_, u);
          if (timedOut_) return have;  // run() discards partial iterations
        }
        worst = std::min(worst, v);
        if (worst <= alpha) break;  // fail low: value is an upper bound
      }
      rootValueMap_[m] = worst;  // bounded value still orders the next pass
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

SearchResult searchV2(const GameState& g, const std::string& myId,
                      const SearchOptions& opts) {
  Searcher s(g, myId, opts);
  return s.run();
}

SearchResult searchV2(const GameState& g, const std::string& myId, double timeBudgetMs) {
  SearchOptions opts;
  opts.budgetMs = timeBudgetMs;
  return searchV2(g, myId, opts);
}

Dir chooseMoveV2(const GameState& g, const std::string& myId, double timeBudgetMs) {
  return searchV2(g, myId, timeBudgetMs).bestMove;
}

}  // namespace bs
