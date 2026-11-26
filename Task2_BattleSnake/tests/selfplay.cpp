// Self-play harness: runs full 11x11 standard-rules games with the local
// rules engine between selectable strategies:
//
//   baseline  starter-level snake (walls + all body cells blocked, greedy
//             food by Manhattan, no H2H/flood-fill awareness)
//   v1        survival core (legal filter, pessimistic flood fill, food
//             clock, tail chase)
//   v2        paranoid minimax + iterative deepening (--v2-budget-ms)
//   snek2     the cloned snek-two opponent (2018 Advanced Division winner)
//             via bench/snek_two_bridge; --snek2-cmd overrides the command
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
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "httplib.h"

#include "rules.hpp"
#include "search.hpp"
#include "strategy.hpp"

using namespace bs;
using json = nlohmann::json;

namespace {

enum class Strat { Baseline, V1, V2, Snek2, Remote };

Strat parseStrat(const char* s) {
  if (!std::strcmp(s, "v1")) return Strat::V1;
  if (!std::strcmp(s, "v2")) return Strat::V2;
  if (!std::strcmp(s, "snek2")) return Strat::Snek2;
  if (!std::strncmp(s, "http://", 7)) return Strat::Remote;
  return Strat::Baseline;
}

const char* stratName(Strat s) {
  switch (s) {
    case Strat::V1: return "v1";
    case Strat::V2: return "v2";
    case Strat::Snek2: return "snek2";
    case Strat::Remote: return "remote";
    default: return "baseline";
  }
}

double g_v2BudgetMs = 25.0;
std::string g_snek2Cmd = "bench/snek_two_bridge";

// --- generic HTTP opponent: any real Battlesnake server (current API) ---
struct RemoteTarget {
  std::string host = "127.0.0.1";
  int port = 80;
};
RemoteTarget g_heroRemote, g_oppRemote;

RemoteTarget parseHttpUrl(const char* url) {
  RemoteTarget t;
  const char* hostStart = url + 7;  // skip "http://"
  const char* colon = std::strchr(hostStart, ':');
  const char* slash = std::strchr(hostStart, '/');
  const char* hostEnd = colon ? colon : slash;
  if (hostEnd)
    t.host.assign(hostStart, hostEnd - hostStart);
  else
    t.host = hostStart;
  if (colon && (!slash || colon < slash)) t.port = std::atoi(colon + 1);
  return t;
}

// --- snek-two bridge: 2018-era API, subprocess per move ---
std::map<std::string, Dir> g_snek2Prev;  // last accepted move, per snake id

// The 2018 API used screen coordinates (y grows downward); today's API has
// y growing upward. We mirror the board vertically so their engine's
// geometry matches ours; the returned move strings then mean the same
// visual direction in both frames.
json point2018(const Point& p, int height) {
  return json{{"object", "point"}, {"x", p.x}, {"y", (height - 1) - p.y}};
}

// Serializes the state into the schema their parser expects:
// world / pointlist / snake objects, snakes list contains alive snakes only.
json stateTo2018Json(const GameState& g, const std::string& youId) {
  json foodData = json::array();
  for (const Point& f : g.food) foodData.push_back(point2018(f, g.height));
  json snakeData = json::array();
  for (const SnakeState& s : g.snakes) {
    if (!s.alive) continue;
    json bodyData = json::array();
    for (const Point& b : s.body) bodyData.push_back(point2018(b, g.height));
    snakeData.push_back(json{{"object", "snake"},
                             {"id", s.id},
                             {"health", s.health},
                             {"body", json{{"object", "pointlist"}, {"data", bodyData}}}});
  }
  return json{{"object", "world"},
              {"width", g.width},
              {"height", g.height},
              {"turn", g.turn},
              {"food", json{{"object", "pointlist"}, {"data", foodData}}},
              {"snakes", json{{"object", "snakelist"}, {"data", snakeData}}},
              {"you", json{{"id", youId}}}};
}

Dir dirFrom2018String(const std::string& s) {
  if (s == "up") return Dir::Up;
  if (s == "down") return Dir::Down;
  if (s == "left") return Dir::Left;
  if (s == "right") return Dir::Right;
  throw std::runtime_error("bad move string");
}

Dir snek2Move(const GameState& g, const std::string& id) {
  Dir prev = Dir::Up;
  auto it = g_snek2Prev.find(id);
  if (it != g_snek2Prev.end()) prev = it->second;
  try {
    const json req = stateTo2018Json(g, id);
    {
      std::ofstream out("/tmp/bs_req.json");
      out << req.dump();
    }
    const int rc = std::system((g_snek2Cmd +
                                " < /tmp/bs_req.json > /tmp/bs_resp.json 2>/dev/null")
                                   .c_str());
    if (rc != 0) return prev;  // timeout/crash: engine semantics = repeat
    std::ifstream in("/tmp/bs_resp.json");
    std::string line;
    while (std::getline(in, line)) {          // their parser prints noise; the
      if (!line.empty() && line[0] == '{') {  // response line starts with '{'
        const json resp = json::parse(line);
        Dir d = dirFrom2018String(resp.at("move").get<std::string>());
        g_snek2Prev[id] = d;
        return d;
      }
    }
  } catch (const std::exception&) {
  }
  return prev;
}

// --- generic HTTP opponent (current Battlesnake API) ---
// Serializes the game state exactly as the real engine would, POSTs it to
// the opponent's /move, and repeats the previous move on any failure
// (which is exactly what the real engine does with a late response).
json pointWire(const Point& p) { return json{{"x", p.x}, {"y", p.y}}; }

json stateToWireJson(const GameState& g, const std::string& youId) {
  json foodArr = json::array();
  for (const Point& f : g.food) foodArr.push_back(pointWire(f));
  json snakesArr = json::array();
  json you;
  for (const SnakeState& s : g.snakes) {
    if (!s.alive) continue;
    json bodyArr = json::array();
    for (const Point& b : s.body) bodyArr.push_back(pointWire(b));
    json sj;
    sj["id"] = s.id;
    sj["name"] = s.id;
    sj["health"] = s.health;
    sj["body"] = bodyArr;
    sj["head"] = pointWire(s.body[0]);  // legacy snakes read it; modern ones ignore
    sj["latency"] = "";
    sj["shout"] = "";
    sj["squad"] = "";
    sj["length"] = static_cast<int>(s.body.size());
    if (s.id == youId) you = sj;
    snakesArr.push_back(std::move(sj));
  }
  json req;
  req["game"]["id"] = "harness-local";
  req["game"]["ruleset"]["name"] = "standard";
  req["game"]["ruleset"]["version"] = "v1";
  req["game"]["timeout"] = 500;
  req["turn"] = g.turn;
  req["board"]["height"] = g.height;
  req["board"]["width"] = g.width;
  req["board"]["food"] = foodArr;
  req["board"]["hazards"] = json::array();
  req["board"]["snakes"] = snakesArr;
  req["you"] = you;
  return req;
}

Dir remoteHttpMove(const GameState& g, const std::string& id, const RemoteTarget& t) {
  Dir prev = Dir::Up;
  auto it = g_snek2Prev.find(id);
  if (it != g_snek2Prev.end()) prev = it->second;
  try {
    const json req = stateToWireJson(g, id);
    httplib::Client cli(t.host, t.port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    const auto res = cli.Post("/move", req.dump(), "application/json");
    if (res && res->status == 200) {
      const json resp = json::parse(res->body);
      Dir d = dirFrom2018String(resp.at("move").get<std::string>());
      g_snek2Prev[id] = d;
      return d;
    }
    std::fprintf(stderr, "remote %s:%d error: status %d\n", t.host.c_str(), t.port,
                 res ? res->status : -1);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "remote %s:%d exception: %s\n", t.host.c_str(), t.port, e.what());
  }
  return prev;
}

Dir chooseFor(Strat st, const GameState& g, const std::string& id) {
  switch (st) {
    case Strat::V1:
      return chooseMoveV1(g, id);
    case Strat::V2:
      return chooseMoveV2(g, id, g_v2BudgetMs);
    case Strat::Snek2:
      return snek2Move(g, id);
    case Strat::Remote:
      return remoteHttpMove(g, id, id == "hero" ? g_heroRemote : g_oppRemote);
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
  g_snek2Prev.clear();  // remote snake repeats "up" until it answers once

  // The no-avoidable-death guarantee only applies to engines we control;
  // a remote opponent may legitimately self-collide.
  const bool heroIsOurs = heroStrat == Strat::V1 || heroStrat == Strat::V2;
  const int kMaxTurns = 800;
  int heroLastLegalCount = -1;
  std::string lossLog;  // move log, printed only when the hero loses
  while (g.aliveCount() > 1 && g.turn < kMaxTurns) {
    std::vector<Dir> moves(g.snakes.size(), Dir::Up);
    for (size_t i = 0; i < g.snakes.size(); ++i) {
      const SnakeState& s = g.snakes[i];
      if (!s.alive) continue;
      if (s.id == heroId) {
        if (heroIsOurs) {
          int legalCount = 0;
          for (const MoveEval& e : evaluateMoves(g, heroId))
            if (e.legal) ++legalCount;
          heroLastLegalCount = legalCount;
        }
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
    {
      char line[256];
      std::snprintf(line, sizeof(line), "turn %d:", g.turn);
      lossLog += line;
      for (size_t i = 0; i < g.snakes.size(); ++i) {
        std::snprintf(line, sizeof(line), " %s@(%d,%d)->%s h%d len%zu%s",
                      g.snakes[i].id.c_str(),
                      g.snakes[i].body.empty() ? -1 : g.snakes[i].head().x,
                      g.snakes[i].body.empty() ? -1 : g.snakes[i].head().y,
                      dirToString(moves[i]), g.snakes[i].health,
                      g.snakes[i].body.size(), g.snakes[i].alive ? "" : " DEAD");
        lossLog += line;
      }
      lossLog += '\n';
      if (std::getenv("BS_DUMP")) std::fputs(lossLog.substr(lossLog.size() - 1).c_str(), stdout);
    }
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
    if (std::getenv("BS_DUMP_LOSSES")) {
      std::printf("=== LOST GAME seed=%u cause=%s, %d turns, final food %zu ===\n%s",
                  seed, elimCauseToString(g.snakes[heroIdx].cause), g.turn,
                  g.food.size(), lossLog.c_str());
    }
    const ElimCause c = g.snakes[heroIdx].cause;
    st.heroDeathCauses[elimCauseToString(c)]++;
    if (heroIsOurs &&
        (c == ElimCause::OutOfBounds || c == ElimCause::SelfCollision ||
         c == ElimCause::Collision)) {
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
    if (!std::strcmp(argv[i], "--a") && i + 1 < argc) {
      heroStrat = parseStrat(argv[i + 1]);
      if (heroStrat == Strat::Remote) g_heroRemote = parseHttpUrl(argv[i + 1]);
      ++i;
    }
    if (!std::strcmp(argv[i], "--b") && i + 1 < argc) {
      oppStrat = parseStrat(argv[i + 1]);
      if (oppStrat == Strat::Remote) g_oppRemote = parseHttpUrl(argv[i + 1]);
      ++i;
    }
    if (!std::strcmp(argv[i], "--v2-budget-ms") && i + 1 < argc)
      v2Budget = std::atof(argv[++i]);
    if (!std::strcmp(argv[i], "--snek2-cmd") && i + 1 < argc)
      g_snek2Cmd = argv[++i];
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
  // Only enforced for engines we control; a remote hero is report-only.
  const bool isV2 = heroStrat == Strat::V2;
  const bool enforce = heroStrat == Strat::V1 || heroStrat == Strat::V2;
  const double threshold = numSnakes == 2 ? (isV2 ? 0.70 : 0.75) : 0.40;
  if (st.filterBug) {
    std::printf("FAILED: hero died by a preventable cause with a legal move "
                "available (filter bug, seed=%u)\n",
                st.filterBugSeed);
    return 1;
  }
  const double winRate = st.heroWins / total;
  if (enforce && winRate < threshold) {
    std::printf("FAILED: hero win rate %.1f%% below %.0f%% threshold\n", 100.0 * winRate,
                100.0 * threshold);
    return 1;
  }
  std::printf(enforce ? "PASSED\n" : "REPORT ONLY (remote hero)\n");
  return 0;
}
