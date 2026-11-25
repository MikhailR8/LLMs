// Self-play harness: runs full 11x11 standard-rules games with the local
// rules engine, pitting v1 against a starter-level baseline.
//
// The baseline models a typical first-week snake: it avoids walls and any
// body cell (including tails - it does not know tails vacate), moves
// greedily toward the nearest food by Manhattan distance, and otherwise
// keeps its current direction. No head-to-head awareness, no flood fill.
//
// Success criteria (this is a test, not a demo):
//   - v1 wins the large majority of duels against the baseline
//   - v1 NEVER dies by out-of-bounds, self-collision or body collision
//     (those would be bugs in the safety filter); only head-to-head and
//     starvation losses are legal outcomes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "rules.hpp"
#include "strategy.hpp"

using namespace bs;

namespace {

// ---------------- starter-level baseline ----------------
Dir chooseBaseline(const GameState& g, const std::string& myId) {
  const int meIdx = g.indexOf(myId);
  if (meIdx < 0 || !g.snakes[meIdx].alive) return Dir::Up;
  const SnakeState& me = g.snakes[meIdx];
  const Point head = me.head();

  // Naive: every body cell of every snake blocks, tails included.
  auto blockedAll = [&g](const Point& p) {
    for (const auto& s : g.snakes) {
      if (!s.alive) continue;
      for (const auto& b : s.body)
        if (b == p) return true;
    }
    return false;
  };

  Dir order[4] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};
  // Prefer keeping the current direction.
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
  if (legal.empty()) return Dir::Up;  // doomed

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

GameState initGame(std::mt19937& rng, int numSnakes, const std::string& v1Id) {
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
    s.id = (i == 0) ? v1Id : ("baseline-" + std::to_string(i));
    s.body = {p, p, p};  // coiled start, stretches over the first turns
    s.health = 100;
    g.snakes.push_back(std::move(s));
  }
  // A couple of initial foods; the per-turn spawn logic maintains the rest.
  for (int k = 0; k < numSnakes; ++k) {
    Point f{static_cast<int>(rng() % 11), static_cast<int>(rng() % 11)};
    if (!g.isBodyAt(f) && !g.isFoodAt(f)) g.food.push_back(f);
  }
  return g;
}

struct RunStats {
  int v1Wins = 0;
  int baseWins = 0;
  int draws = 0;
  long totalTurns = 0;
  std::map<std::string, int> v1DeathCauses;
  // Died with zero legal moves available (enclosed): any move loses, the
  // elimination cause just reflects which doomed move was taken.
  std::map<std::string, int> v1EnclosedDeaths;
  bool filterBug = false;
  uint32_t filterBugSeed = 0;
};

void playGame(uint32_t seed, int numSnakes, RunStats& st, bool v1First) {
  std::mt19937 rng(seed);
  const std::string v1Id = "v1";
  GameState g = initGame(rng, numSnakes, v1Id);
  if (!v1First) std::swap(g.snakes[0], g.snakes[1]);  // vary colors/seats

  const int kMaxTurns = 800;
  int v1LastLegalCount = -1;
  while (g.aliveCount() > 1 && g.turn < kMaxTurns) {
    std::vector<Dir> moves(g.snakes.size(), Dir::Up);
    for (size_t i = 0; i < g.snakes.size(); ++i) {
      const SnakeState& s = g.snakes[i];
      if (!s.alive) continue;
      if (s.id == v1Id) {
        int legalCount = 0;
        for (const MoveEval& e : evaluateMoves(g, v1Id))
          if (e.legal) ++legalCount;
        v1LastLegalCount = legalCount;
        moves[i] = chooseMoveV1(g, v1Id);
      } else {
        moves[i] = chooseBaseline(g, s.id);
      }
    }
    applyTurn(g, moves, rng);
  }

  st.totalTurns += g.turn;
  const int alive = g.aliveCount();
  const int v1Idx = g.indexOf(v1Id);
  if (alive == 0) {
    ++st.draws;
  } else if (g.snakes[v1Idx].alive && alive == 1) {
    ++st.v1Wins;
  } else {
    ++st.baseWins;
    const ElimCause c = g.snakes[v1Idx].cause;
    st.v1DeathCauses[elimCauseToString(c)]++;
    if (c == ElimCause::OutOfBounds || c == ElimCause::SelfCollision ||
        c == ElimCause::Collision) {
      if (v1LastLegalCount > 0) {
        // A legal alternative existed: the safety filter failed for real.
        st.filterBug = true;
        st.filterBugSeed = seed;
      } else {
        st.v1EnclosedDeaths[elimCauseToString(c)]++;
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  int games = 200;
  const char* mode = "duel";
  uint32_t baseSeed = 1;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--games") && i + 1 < argc) games = std::atoi(argv[++i]);
    if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
    if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) baseSeed = static_cast<uint32_t>(std::atoi(argv[++i]));
  }
  const int numSnakes = std::strcmp(mode, "ffa") == 0 ? 4 : 2;

  RunStats st;
  for (int i = 0; i < games; ++i)
    playGame(baseSeed + static_cast<uint32_t>(i), numSnakes, st, (i % 2) == 0);

  const double total = static_cast<double>(games);
  std::printf("mode=%s games=%d\n", mode, games);
  std::printf("  v1 wins:        %d (%.1f%%)\n", st.v1Wins, 100.0 * st.v1Wins / total);
  std::printf("  baseline wins:  %d (%.1f%%)\n", st.baseWins, 100.0 * st.baseWins / total);
  std::printf("  draws:          %d (%.1f%%)\n", st.draws, 100.0 * st.draws / total);
  std::printf("  avg game turns: %.1f\n", st.totalTurns / total);
  if (!st.v1DeathCauses.empty()) {
    std::printf("  v1 loss causes:");
    for (const auto& kv : st.v1DeathCauses)
      std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf("\n");
  }
  if (!st.v1EnclosedDeaths.empty()) {
    int enclosed = 0;
    for (const auto& kv : st.v1EnclosedDeaths) enclosed += kv.second;
    std::printf("  v1 enclosed (no legal move, death was unavoidable): %d\n", enclosed);
  }

  if (st.filterBug) {
    std::printf("FAILED: v1 died by a preventable cause with a legal move "
                "available (filter bug, seed=%u)\n",
                st.filterBugSeed);
    return 1;
  }
  const double winRate = st.v1Wins / total;
  const double threshold = (numSnakes == 2) ? 0.75 : 0.40;  // 25% is chance in FFA
  if (winRate < threshold) {
    std::printf("FAILED: v1 win rate %.1f%% below %.0f%% threshold\n", 100.0 * winRate,
                100.0 * threshold);
    return 1;
  }
  std::printf("PASSED\n");
  return 0;
}
