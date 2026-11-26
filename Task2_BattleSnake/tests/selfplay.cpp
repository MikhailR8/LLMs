// Self-play harness: runs full 11x11 standard-rules games with the local
// rules engine between selectable strategies:
//
//   baseline  starter-level snake (walls + all body cells blocked, greedy
//             food by Manhattan, no H2H/flood-fill awareness)
//   v1        survival core (legal filter, pessimistic flood fill, food
//             clock, tail chase)
//   v2        paranoid minimax + iterative deepening (--v2-budget-ms)
//
// Success criteria (this is a test, not a demo):
//   - the hero (--a) wins the large majority of its games
//   - the hero NEVER dies by out-of-bounds, self-collision or body
//     collision while a legal move existed (safety-filter bug); only
//     head-to-head, starvation and fully-enclosed deaths are legal outcomes.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "rules.hpp"
#include "search.hpp"
#include "strategy.hpp"

using namespace bs;

namespace {

enum class Strat { Baseline, V1, V2 };

Strat parseStrat(const char* s) {
  if (!std::strcmp(s, "v1")) return Strat::V1;
  if (!std::strcmp(s, "v2")) return Strat::V2;
  return Strat::Baseline;
}

const char* stratName(Strat s) {
  switch (s) {
    case Strat::V1: return "v1";
    case Strat::V2: return "v2";
    default: return "baseline";
  }
}

double g_v2BudgetMs = 25.0;

Dir chooseFor(Strat st, const GameState& g, const std::string& id) {
  switch (st) {
    case Strat::V1:
      return chooseMoveV1(g, id);
    case Strat::V2:
      return chooseMoveV2(g, id, g_v2BudgetMs);
    default:
      break;
  }
  // ---- starter-level baseline ----
  const int meIdx = g.indexOf(id);
  if (meIdx < 0 || !g.snakes[meIdx].alive) return Dir::Up;
  const SnakeState& me = g.snakes[meIdx];
  const Point head = me.head();

  auto blockedAll = [&g](const Point& p) {
    for (const auto& s : g.snakes) {
      if (!s.alive) continue;
      for (const auto& b : s.body)
        if (b == p) return true;
    }
    return false;
  };

  Dir order[4] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};
  if (me.body.size() >= 2) {
    Dir keep = dirFromNeck(me.head(), me.body[1]);
    for (int i = 0; i < 4; ++i)
      if (order[i] == keep) std::swap(order[0], order[i]);
  }

  std::vector<Dir> legal;
  for (Dir d : order) {
    Point nh = step(head, d);
    if (g.inBounds(nh) && !blockedAll(nh)) legal.push_back(d);
  }
  if (legal.empty()) return Dir::Up;

  if (!g.food.empty()) {
    const Point& target = g.food[0];
    int bestDist = 1 << 30;
    Dir best = legal[0];
    for (Dir d : legal) {
      Point nh = step(head, d);
      int dist = std::abs(target.x - nh.x) + std::abs(target.y - nh.y);
      if (dist < bestDist) {
        bestDist = dist;
        best = d;
      }
    }
    return best;
  }
  return legal[0];
}

// ---------------- game setup ----------------
std::vector<Point> kDuelSpawns = {{3, 5}, {7, 5}};
std::vector<Point> kFfaSpawns = {{2, 2}, {8, 2}, {2, 8}, {8, 8}};

GameState initGame(std::mt19937& rng, int numSnakes, const std::string& heroId) {
  GameState g;
  g.width = 11;
  g.height = 11;
  g.foodSpawnChance = 15;
  g.minimumFood = 1;

  const std::vector<Point>& spawns = (numSnakes == 2) ? kDuelSpawns : kFfaSpawns;
  for (int i = 0; i < numSnakes; ++i) {
    Point p = spawns[i % spawns.size()];
    p.x = std::min(10, std::max(0, p.x + static_cast<int>(rng() % 3) - 1));
    p.y = std::min(10, std::max(0, p.y + static_cast<int>(rng() % 3) - 1));
    SnakeState s;
    s.id = (i == 0) ? heroId : ("opponent-" + std::to_string(i));
    s.body = {p, p, p};  // coiled start, stretches over the first turns
    s.health = 100;
    g.snakes.push_back(std::move(s));
  }
  for (int k = 0; k < numSnakes; ++k) {
    Point f{static_cast<int>(rng() % 11), static_cast<int>(rng() % 11)};
    if (!g.isBodyAt(f) && !g.isFoodAt(f)) g.food.push_back(f);
  }
  return g;
}

struct RunStats {
  int heroWins = 0;
  int oppWins = 0;
  int draws = 0;
  long totalTurns = 0;
  double heroComputeMs = 0.0;
  double heroMaxComputeMs = 0.0;
  long heroComputeCalls = 0;
  int heroDepthSum = 0;
  int heroDepthMax = 0;
  std::map<std::string, int> heroDeathCauses;
  std::map<std::string, int> heroEnclosedDeaths;
  bool filterBug = false;
  uint32_t filterBugSeed = 0;
};

void playGame(uint32_t seed, int numSnakes, Strat heroStrat, Strat oppStrat,
              RunStats& st, bool heroFirst) {
  std::mt19937 rng(seed);
  const std::string heroId = "hero";
  GameState g = initGame(rng, numSnakes, heroId);
  if (!heroFirst) std::swap(g.snakes[0], g.snakes[1]);  // vary seats

  const int kMaxTurns = 800;
  int heroLastLegalCount = -1;
  while (g.aliveCount() > 1 && g.turn < kMaxTurns) {
    std::vector<Dir> moves(g.snakes.size(), Dir::Up);
    for (size_t i = 0; i < g.snakes.size(); ++i) {
      const SnakeState& s = g.snakes[i];
      if (!s.alive) continue;
      if (s.id == heroId) {
        int legalCount = 0;
        for (const MoveEval& e : evaluateMoves(g, heroId))
          if (e.legal) ++legalCount;
        heroLastLegalCount = legalCount;
        const auto t0 = std::chrono::steady_clock::now();
        if (heroStrat == Strat::V2) {
          const SearchResult r = searchV2(g, heroId, g_v2BudgetMs);
          moves[i] = r.bestMove;
          const double ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
          st.heroComputeMs += ms;
          st.heroMaxComputeMs = std::max(st.heroMaxComputeMs, ms);
          ++st.heroComputeCalls;
          st.heroDepthSum += r.depthReached;
          st.heroDepthMax = std::max(st.heroDepthMax, r.depthReached);
        } else {
          moves[i] = chooseFor(heroStrat, g, heroId);
          st.heroComputeMs += std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
          ++st.heroComputeCalls;
        }
      } else {
        moves[i] = chooseFor(oppStrat, g, s.id);
      }
    }
    applyTurn(g, moves, rng);
  }

  st.totalTurns += g.turn;
  const int alive = g.aliveCount();
  const int heroIdx = g.indexOf(heroId);
  if (alive == 0) {
    ++st.draws;
  } else if (g.snakes[heroIdx].alive && alive == 1) {
    ++st.heroWins;
  } else {
    ++st.oppWins;
    const ElimCause c = g.snakes[heroIdx].cause;
    st.heroDeathCauses[elimCauseToString(c)]++;
    if (c == ElimCause::OutOfBounds || c == ElimCause::SelfCollision ||
        c == ElimCause::Collision) {
      if (heroLastLegalCount > 0) {
        st.filterBug = true;
        st.filterBugSeed = seed;
      } else {
        st.heroEnclosedDeaths[elimCauseToString(c)]++;
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  int games = 200;
  const char* mode = "duel";
  uint32_t baseSeed = 1;
  Strat heroStrat = Strat::V1;
  Strat oppStrat = Strat::Baseline;
  double v2Budget = 25.0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--games") && i + 1 < argc) games = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
    if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
      baseSeed = static_cast<uint32_t>(std::atoi(argv[++i]));
    if (!std::strcmp(argv[i], "--a") && i + 1 < argc) heroStrat = parseStrat(argv[++i]);
    if (!std::strcmp(argv[i], "--b") && i + 1 < argc) oppStrat = parseStrat(argv[++i]);
    if (!std::strcmp(argv[i], "--v2-budget-ms") && i + 1 < argc)
      v2Budget = std::atof(argv[++i]);
  }
  g_v2BudgetMs = v2Budget;
  const int numSnakes = std::strcmp(mode, "ffa") == 0 ? 4 : 2;

  RunStats st;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < games; ++i)
    playGame(baseSeed + static_cast<uint32_t>(i), numSnakes, heroStrat, oppStrat, st,
             (i % 2) == 0);
  const double wallSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const double total = static_cast<double>(games);
  std::printf("mode=%s games=%d hero=%s opp=%s v2Budget=%.0fms\n", mode, games,
              stratName(heroStrat), stratName(oppStrat), v2Budget);
  std::printf("  hero wins:      %d (%.1f%%)\n", st.heroWins, 100.0 * st.heroWins / total);
  std::printf("  opp wins:       %d (%.1f%%)\n", st.oppWins, 100.0 * st.oppWins / total);
  std::printf("  draws:          %d (%.1f%%)\n", st.draws, 100.0 * st.draws / total);
  std::printf("  avg game turns: %.1f (harness %.0fs)\n", st.totalTurns / total, wallSec);
  if (st.heroComputeCalls > 0) {
    std::printf("  hero compute:   avg %.2fms, max %.2fms, avg depth %.1f, max depth %d\n",
                st.heroComputeMs / st.heroComputeCalls, st.heroMaxComputeMs,
                static_cast<double>(st.heroDepthSum) / st.heroComputeCalls,
                st.heroDepthMax);
  }
  if (!st.heroDeathCauses.empty()) {
    std::printf("  hero loss causes:");
    for (const auto& kv : st.heroDeathCauses)
      std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf("\n");
  }
  if (!st.heroEnclosedDeaths.empty()) {
    int enclosed = 0;
    for (const auto& kv : st.heroEnclosedDeaths) enclosed += kv.second;
    std::printf("  hero enclosed (no legal move, death was unavoidable): %d\n", enclosed);
  }

  // Thresholds: hero must dominate its opposition (chance in FFA = 25%).
  const bool isV2 = heroStrat == Strat::V2;
  const double threshold = numSnakes == 2 ? (isV2 ? 0.70 : 0.75) : 0.40;
  if (st.filterBug) {
    std::printf("FAILED: hero died by a preventable cause with a legal move "
                "available (filter bug, seed=%u)\n",
                st.filterBugSeed);
    return 1;
  }
  const double winRate = st.heroWins / total;
  if (winRate < threshold) {
    std::printf("FAILED: hero win rate %.1f%% below %.0f%% threshold\n", 100.0 * winRate,
                100.0 * threshold);
    return 1;
  }
  std::printf("PASSED\n");
  return 0;
}
