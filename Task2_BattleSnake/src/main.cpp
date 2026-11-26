// Battlesnake C++ server - v2 search engine (v1 survival core as fallback).
// Same dependency stack as bgaechter/battlesnake-starter-cpp:
// cpp-httplib (yhirose) + nlohmann/json, both vendored in third_party/.
//
// Endpoints: GET / (metadata), POST /start, POST /move, POST /end.
// The move decision is a pure function of the request state, so the server
// is safe under concurrent games. Every failure path still answers with a
// valid move: an invalid response makes the engine repeat the previous move.
//
// Env knobs:
//   BATTLENSNAKE_BUDGET_MS  search time budget per move (default 350; the
//                           engine allows 500ms, headroom covers scheduling)
//   BATTLENSNAKE_STRATEGY   "v2" (default) or "v1"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "rules.hpp"
#include "search.hpp"
#include "strategy.hpp"

using json = nlohmann::json;
using namespace bs;

namespace {

json infoJson() {
  return json{
      {"apiversion", "1"},
      {"author", "zcode"},
      {"color", "#5B8DB8"},
      {"head", "shades"},
      {"tail", "round-bum"},
      {"version", "v2-minimax"},
  };
}

Point pointFromJson(const json& j) { return Point{j.at("x").get<int>(), j.at("y").get<int>()}; }

GameState stateFromJson(const json& j) {
  GameState g;
  const json& game = j.at("game");
  g.turn = j.value("turn", 0);

  std::string rulesetName = game.contains("ruleset") ? game["ruleset"].value("name", "") : "";
  std::string mapName = game.value("map", "");
  if (game.contains("ruleset") && game["ruleset"].contains("settings") &&
      game["ruleset"]["settings"].is_object()) {
    const json& st = game["ruleset"]["settings"];
    if (st.contains("foodSpawnChance") && st["foodSpawnChance"].is_number())
      g.foodSpawnChance = st["foodSpawnChance"].get<int>();
    if (st.contains("minimumFood") && st["minimumFood"].is_number())
      g.minimumFood = st["minimumFood"].get<int>();
    if (st.contains("hazardDamagePerTurn") && st["hazardDamagePerTurn"].is_number())
      g.hazardDamagePerTurn = st["hazardDamagePerTurn"].get<int>();
  }
  // Constrictor: no food spawns and none exists on the board.
  if (rulesetName == "constrictor" || mapName == "constrictor") {
    g.foodSpawning = false;
    g.food.clear();
  }

  const json& board = j.at("board");
  g.width = board.value("width", 11);
  g.height = board.value("height", 11);
  if (board.contains("food") && board["food"].is_array())
    for (const json& f : board["food"]) g.food.push_back(pointFromJson(f));
  if (board.contains("hazards") && board["hazards"].is_array())
    for (const json& h : board["hazards"]) g.hazards.push_back(pointFromJson(h));
  if (board.contains("snakes") && board["snakes"].is_array()) {
    for (const json& s : board["snakes"]) {
      SnakeState ss;
      ss.id = s.at("id").get<std::string>();
      ss.health = s.value("health", 100);
      if (s.contains("body") && s["body"].is_array())
        for (const json& b : s["body"]) ss.body.push_back(pointFromJson(b));
      ss.alive = true;  // the API only lists living snakes
      g.snakes.push_back(std::move(ss));
    }
  }
  return g;
}

}  // namespace

int main() {
  httplib::Server svr;

  double budgetMs = 350.0;
  if (const char* envBudget = std::getenv("BATTLENSNAKE_BUDGET_MS"))
    budgetMs = std::atof(envBudget);
  bool useV2 = true;
  if (const char* envStrat = std::getenv("BATTLENSNAKE_STRATEGY"))
    useV2 = std::string(envStrat) != "v1";
  const double budget = budgetMs;  // captured for the handler below

  svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(infoJson().dump(), "application/json");
  });

  svr.Post("/start", [](const httplib::Request& req, httplib::Response& res) {
    (void)req;
    res.set_content("{}", "application/json");
  });

  svr.Post("/move", [budget, useV2](const httplib::Request& req, httplib::Response& res) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string gameId = "?";
    int turn = -1;
    try {
      const json j = json::parse(req.body);
      if (j.contains("game")) gameId = j["game"].value("id", "?");
      turn = j.value("turn", -1);
      const GameState g = stateFromJson(j);
      const std::string youId = j.at("you").at("id").get<std::string>();

      std::string shout = "v2 minimax";
      Dir d;
      int depth = 0;
      if (useV2) {
        const SearchResult r = searchV2(g, youId, budget);
        d = r.bestMove;
        depth = r.depthReached;
        if (r.fellBackToV1) shout = "v1 fallback";
        if (r.forcedWin) shout = "forced win";
      } else {
        d = chooseMoveV1(g, youId);
        shout = "v1 survival";
      }

      json out = {{"move", bs::dirToString(d)}, {"shout", shout}};
      const auto t1 = std::chrono::steady_clock::now();
      const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      std::fprintf(stderr, "[move] game=%s turn=%d move=%s depth=%d compute=%.3fms\n",
                   gameId.c_str(), turn, bs::dirToString(d), depth, ms);
      res.set_content(out.dump(), "application/json");
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[move] game=%s turn=%d ERROR: %s\n", gameId.c_str(), turn,
                   e.what());
      res.set_content(R"({"move":"up","shout":"fallback"})", "application/json");
    }
  });

  svr.Post("/end", [](const httplib::Request& req, httplib::Response& res) {
    (void)req;
    res.set_content("{}", "application/json");
  });

  int port = 8080;
  if (const char* envPort = std::getenv("PORT")) port = std::atoi(envPort);

  std::fprintf(stderr, "Battlesnake v2 (%s, budget %.0fms) listening on 0.0.0.0:%d\n",
               useV2 ? "search" : "v1", budget, port);
  if (!svr.listen("0.0.0.0", port)) {
    std::fprintf(stderr, "failed to listen on port %d\n", port);
    return 1;
  }
  return 0;
}
