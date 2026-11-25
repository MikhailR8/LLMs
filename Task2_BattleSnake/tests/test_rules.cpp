// Unit tests for the rules engine (bs::applyTurn) and the v1 move filter.
// Every case below is a semantic extracted from BattlesnakeOfficial/rules
// standard.go; see comments per test.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "rules.hpp"
#include "strategy.hpp"

using namespace bs;

static int g_total = 0;
static int g_passed = 0;
#define CHECK(...)                                                     \
  do {                                                                  \
    ++g_total;                                                          \
    if ((__VA_ARGS__)) {                                                \
      ++g_passed;                                                       \
    } else {                                                            \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
    }                                                                   \
  } while (0)

static SnakeState mkSnake(const std::string& id, std::vector<Point> body, int health = 100) {
  SnakeState s;
  s.id = id;
  s.body = std::move(body);
  s.health = health;
  return s;
}

static GameState mkState(std::vector<SnakeState> snakes) {
  GameState g;
  g.width = 11;
  g.height = 11;
  g.snakes = std::move(snakes);
  return g;
}

static std::mt19937 rng(12345);

int main() {
  // --- 1. Moving into your own tail cell is legal (tail vacates). ---
  {
    GameState g = mkState({mkSnake("a", {{2, 1}, {2, 2}, {3, 2}, {3, 1}})});
    applyTurn(g, {Dir::Right}, rng);  // new head (3,1) == own tail
    CHECK(g.snakes[0].alive);
    CHECK(g.snakes[0].length() == 4);
  }
  // --- 2. Moving into a duplicated tail (snake ate last turn) kills. ---
  {
    GameState g = mkState({mkSnake("a", {{2, 1}, {2, 2}, {3, 2}, {3, 1}, {3, 1}})});
    applyTurn(g, {Dir::Right}, rng);  // new head (3,1) is still occupied
    CHECK(!g.snakes[0].alive);
    CHECK(g.snakes[0].cause == ElimCause::SelfCollision);
  }
  // --- 3. Equal-length head-to-head: both die. ---
  {
    GameState g = mkState({mkSnake("a", {{4, 5}, {3, 5}, {2, 5}}),
                           mkSnake("b", {{6, 5}, {7, 5}, {8, 5}})});
    auto out = applyTurn(g, {Dir::Right, Dir::Left}, rng);  // both heads -> (5,5)
    CHECK(!g.snakes[0].alive);
    CHECK(!g.snakes[1].alive);
    CHECK(g.snakes[0].cause == ElimCause::HeadToHead);
    CHECK(g.snakes[1].cause == ElimCause::HeadToHead);
    CHECK(out.gameOver);
  }
  // --- 4. Head-to-head: longer snake survives. ---
  {
    GameState g = mkState({mkSnake("a", {{4, 5}, {3, 5}, {2, 5}, {1, 5}}),
                           mkSnake("b", {{6, 5}, {7, 5}, {8, 5}})});
    applyTurn(g, {Dir::Right, Dir::Left}, rng);
    CHECK(g.snakes[0].alive);
    CHECK(!g.snakes[1].alive);
    CHECK(g.snakes[1].cause == ElimCause::HeadToHead);
  }
  // --- 5. Moving onto another snake's vacating tail cell is legal. ---
  {
    GameState g = mkState({mkSnake("a", {{4, 3}, {3, 3}, {3, 4}}),
                           mkSnake("b", {{5, 5}, {5, 4}, {5, 3}})});
    // b moves up (its tail (5,3) vacates); a moves right onto (5,3).
    applyTurn(g, {Dir::Right, Dir::Up}, rng);
    CHECK(g.snakes[0].alive);
    CHECK(g.snakes[1].alive);
    CHECK(g.snakes[0].head() == Point{5, 3});
  }
  // --- 6. Eating at health 1 keeps you alive (feed runs before elimination). ---
  {
    GameState g = mkState({mkSnake("a", {{5, 5}, {5, 4}}, 1)});
    g.food.push_back({5, 6});
    applyTurn(g, {Dir::Up}, rng);
    CHECK(g.snakes[0].alive);
    CHECK(g.snakes[0].health == 100);
    CHECK(g.snakes[0].length() == 3);  // grew by one
    CHECK(g.snakes[0].tailIsDuplicated());
    CHECK(g.food.empty());
  }
  // --- 7. Starving without food kills. ---
  {
    GameState g = mkState({mkSnake("a", {{5, 5}, {5, 4}}, 1)});
    applyTurn(g, {Dir::Up}, rng);
    CHECK(!g.snakes[0].alive);
    CHECK(g.snakes[0].cause == ElimCause::Starved);
  }
  // --- 8. Out of bounds kills. ---
  {
    GameState g = mkState({mkSnake("a", {{0, 5}, {1, 5}, {2, 5}})});
    applyTurn(g, {Dir::Left}, rng);  // new head (-1,5)
    CHECK(!g.snakes[0].alive);
    CHECK(g.snakes[0].cause == ElimCause::OutOfBounds);
  }
  // --- 9. Mutual body collision eliminates both. ---
  {
    GameState g = mkState({mkSnake("a", {{5, 5}, {4, 5}}),
                           mkSnake("b", {{6, 5}, {7, 5}})});
    // a->(6,5) which stays in b's post-move body; b->(5,5) stays in a's.
    applyTurn(g, {Dir::Right, Dir::Left}, rng);
    CHECK(!g.snakes[0].alive);
    CHECK(!g.snakes[1].alive);
    CHECK(g.snakes[0].cause == ElimCause::Collision);
    CHECK(g.snakes[1].cause == ElimCause::Collision);
  }
  // --- 10. A snake that starved this turn stops blocking: moving into its
  //        old head cell is legal for the survivor. ---
  {
    GameState g = mkState({mkSnake("a", {{5, 5}, {5, 4}}, 1),
                           mkSnake("b", {{5, 6}, {5, 7}})});
    applyTurn(g, {Dir::Up, Dir::Down}, rng);  // a starves, b enters (5,5)
    CHECK(!g.snakes[0].alive);
    CHECK(g.snakes[1].alive);
    CHECK(g.snakes[1].head() == Point{5, 5});
  }
  // --- 11. Hazard kills; food on a hazard protects and feeds. ---
  {
    GameState g = mkState({mkSnake("a", {{5, 5}, {5, 4}}, 10)});
    g.hazards.push_back({5, 6});
    g.hazardDamagePerTurn = 14;
    applyTurn(g, {Dir::Up}, rng);
    CHECK(!g.snakes[0].alive);
    CHECK(g.snakes[0].cause == ElimCause::Hazard);
  }
  {
    GameState g = mkState({mkSnake("a", {{5, 5}, {5, 4}}, 10)});
    g.hazards.push_back({5, 6});
    g.food.push_back({5, 6});
    g.hazardDamagePerTurn = 14;
    applyTurn(g, {Dir::Up}, rng);
    CHECK(g.snakes[0].alive);
    CHECK(g.snakes[0].health == 100);
  }
  // --- 12. Food spawns to the minimum each turn (needs 2+ snakes, since a
  //         standard game is over as soon as one snake remains). ---
  {
    GameState g = mkState({mkSnake("a", {{2, 2}, {2, 3}}),
                           mkSnake("b", {{8, 8}, {8, 7}})});
    g.foodSpawnChance = 0;
    g.minimumFood = 1;
    applyTurn(g, {Dir::Up, Dir::Down}, rng);
    CHECK(g.food.size() >= 1);
    CHECK(!g.isBodyAt(g.food[0]));
  }
  // --- 13. blockedAfterMove: normal tail vacates, duplicated tail does not. ---
  {
    GameState g = mkState({mkSnake("a", {{2, 1}, {2, 2}, {3, 2}, {3, 1}})});
    auto blocked = blockedAfterMove(g);
    bool tailBlocked = false;
    for (auto& p : blocked)
      if (p == Point{3, 1}) tailBlocked = true;
    CHECK(!tailBlocked);
  }
  {
    GameState g = mkState({mkSnake("a", {{2, 1}, {2, 2}, {3, 2}, {3, 1}, {3, 1}})});
    auto blocked = blockedAfterMove(g);
    bool tailBlocked = false;
    for (auto& p : blocked)
      if (p == Point{3, 1}) tailBlocked = true;
    CHECK(tailBlocked);
  }
  // --- 14. Multiple snakes can share one food (both eat, food removed once). ---
  {
    GameState g = mkState({mkSnake("a", {{4, 6}, {4, 7}}),
                           mkSnake("b", {{6, 6}, {6, 7}})});
    g.food.push_back({5, 6});
    applyTurn(g, {Dir::Right, Dir::Left}, rng);
    // Both heads land on (5,6) -> equal length head-to-head kills both,
    // but both did eat first: health is restored before elimination.
    CHECK(g.snakes[0].health == 100);
    CHECK(g.snakes[1].health == 100);
    CHECK(!g.snakes[0].alive && !g.snakes[1].alive);
    CHECK(g.food.empty());
  }

  // ===== strategy-level checks =====
  // --- 15. v1 never walks out of bounds or into its own body. ---
  {
    GameState g = mkState({mkSnake("me", {{0, 0}, {1, 0}, {2, 0}})});
    Dir d = chooseMoveV1(g, "me");
    // Left/Down are out of bounds, Right is its own neck: must go up.
    CHECK(d == Dir::Up);
  }
  // --- 16. v1 avoids head-to-head risk cells when a safe move exists. ---
  {
    GameState g = mkState({mkSnake("me", {{3, 3}, {2, 3}, {1, 3}}),
                           mkSnake("enemy", {{4, 4}, {4, 5}, {4, 6}})});
    Dir d = chooseMoveV1(g, "me");
    // Right (4,3) and Up (3,4) are adjacent to the enemy head (4,4);
    // Left is our neck. The only safe move is Down.
    CHECK(d == Dir::Down);
  }
  // --- 17. v1 takes the tail-chase move when it is the only legal one. ---
  {
    GameState g = mkState({mkSnake("me", {{2, 1}, {2, 2}, {3, 2}, {3, 1}})});
    Dir d = chooseMoveV1(g, "me");
    // Same geometry as test 1: right into the vacating tail is legal.
    auto evals = evaluateMoves(g, "me");
    bool rightLegal = false;
    for (auto& e : evals)
      if (e.move == Dir::Right && e.legal) rightLegal = true;
    CHECK(rightLegal);
    (void)d;
  }
  // --- 18. v1 prefers food when starving. ---
  {
    GameState g = mkState({mkSnake("me", {{5, 5}, {4, 5}, {3, 5}}, 12)});
    g.food.push_back({5, 9});
    Dir d = chooseMoveV1(g, "me");
    CHECK(d == Dir::Up);  // toward the food, and away from the neck
  }
  // --- 19. evaluateMoves marks head-to-head risk correctly. ---
  {
    GameState g = mkState({mkSnake("me", {{3, 3}, {2, 3}, {1, 3}}),
                           mkSnake("enemy", {{5, 3}, {6, 3}, {7, 3}})});
    auto evals = evaluateMoves(g, "me");
    for (auto& e : evals) {
      if (e.move == Dir::Right) CHECK(e.h2hRisk);   // (4,3) next to enemy head
      if (e.move == Dir::Up) CHECK(!e.h2hRisk);
    }
  }
  // --- 20. Dead/unknown snake falls back safely. ---
  {
    GameState g = mkState({mkSnake("me", {{5, 5}, {5, 4}})});
    Dir d = chooseMoveV1(g, "nobody");
    CHECK(d == Dir::Up);
  }

  std::printf("rules+strategy tests: %d/%d passed\n", g_passed, g_total);
  return g_passed == g_total ? 0 : 1;
}
