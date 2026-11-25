// Battlesnake rules engine - faithful C++ port of BattlesnakeOfficial/rules
// standard.go (turn resolution, elimination, feeding, food spawning).
// This is the single source of truth for game semantics in this project.
#pragma once

#include <random>
#include <string>
#include <vector>

namespace bs {

struct Point {
  int x = 0;
  int y = 0;
  bool operator==(const Point& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Point& o) const { return !(*this == o); }
};

enum class Dir : uint8_t { Up = 0, Down, Left, Right };
constexpr int kNumDirs = 4;

const char* dirToString(Dir d);
// Returns true on success; accepts "up"/"down"/"left"/"right".
bool dirFromString(const std::string& s, Dir& out);
Point step(const Point& p, Dir d);
// Direction implied by a neck->head vector (used to repeat the previous move).
Dir dirFromNeck(const Point& head, const Point& neck);

enum class ElimCause : uint8_t {
  NotEliminated = 0,
  Starved,        // health reached zero
  OutOfBounds,
  SelfCollision,
  Collision,      // ran into another snake's body
  HeadToHead,     // lost or tied a head-to-head
  Hazard,         // hazard damage brought health to zero
};
const char* elimCauseToString(ElimCause c);

struct SnakeState {
  std::string id;
  std::vector<Point> body;  // head-first; may carry a duplicated tail after eating
  int health = 100;
  bool alive = true;
  ElimCause cause = ElimCause::NotEliminated;
  std::string eliminatedBy;

  int length() const { return static_cast<int>(body.size()); }
  const Point& head() const { return body[0]; }
  const Point& tail() const { return body.back(); }
  // Eating appends a duplicate of the current tail; while that duplicate is
  // present the tail cell does NOT vacate on the next move.
  bool tailIsDuplicated() const {
    return body.size() >= 2 && body.back() == body[body.size() - 2];
  }
};

struct GameState {
  int width = 11;
  int height = 11;
  int turn = 0;

  // Ruleset settings (defaults follow the standard ladder settings).
  int foodSpawnChance = 15;       // percent chance per turn
  int minimumFood = 1;            // kept on the board at all times
  int hazardDamagePerTurn = 0;    // 0 = no hazards (standard); royale = 14
  bool foodSpawning = true;       // false for constrictor

  std::vector<Point> food;
  std::vector<Point> hazards;
  std::vector<SnakeState> snakes;

  int aliveCount() const;
  int indexOf(const std::string& id) const;  // -1 when absent
  bool inBounds(const Point& p) const {
    return p.x >= 0 && p.x < width && p.y >= 0 && p.y < height;
  }
  bool isFoodAt(const Point& p) const;
  bool isHazardAt(const Point& p) const;
  bool isBodyAt(const Point& p) const;  // any alive snake, any segment
};

struct TurnOutcome {
  bool gameOver = false;
  std::vector<int> eliminatedIdx;  // indices into GameState::snakes
};

// Cells that stay occupied no matter how the snakes move this turn:
// every alive snake's body[0 .. len-2]. The vacating tail (body[len-1]) is
// NOT included, unless it is duplicated (snake ate last turn).
// This is exactly the set a planner must treat as blocked: a snake's
// post-move body[1..] equals its current body[0..len-2].
std::vector<Point> blockedAfterMove(const GameState& g);

// Applies one full turn in the exact order of standard.go:
//   1. movement  (new head pushed, tail popped - always)
//   2. health -1 for every alive snake
//   3. hazard damage (skipped when food sits on the hazard cell;
//      may eliminate immediately, which also skips feeding)
//   4. feeding  (head on food: health = 100, duplicate tail appended,
//      food removed; several snakes may share one food)
//   5. elimination, two phases: first health/out-of-bounds (those bodies
//      stop blocking immediately), then self-collision / body-collision /
//      head-to-head collected among the survivors and applied together
//   6. game-over check (alive <= 1)
//   7. food spawn (map logic: refill to minimum, else chance roll)
// moves[i] is the move of snakes[i]; entries for dead snakes are ignored.
TurnOutcome applyTurn(GameState& g, const std::vector<Dir>& moves, std::mt19937& rng);

}  // namespace bs
