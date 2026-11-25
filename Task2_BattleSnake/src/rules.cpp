#include "rules.hpp"

#include <algorithm>

namespace bs {

const char* dirToString(Dir d) {
  switch (d) {
    case Dir::Up: return "up";
    case Dir::Down: return "down";
    case Dir::Left: return "left";
    case Dir::Right: return "right";
  }
  return "up";
}

bool dirFromString(const std::string& s, Dir& out) {
  if (s == "up") { out = Dir::Up; return true; }
  if (s == "down") { out = Dir::Down; return true; }
  if (s == "left") { out = Dir::Left; return true; }
  if (s == "right") { out = Dir::Right; return true; }
  return false;
}

Point step(const Point& p, Dir d) {
  Point q = p;
  switch (d) {
    case Dir::Up: q.y += 1; break;
    case Dir::Down: q.y -= 1; break;
    case Dir::Left: q.x -= 1; break;
    case Dir::Right: q.x += 1; break;
  }
  return q;
}

Dir dirFromNeck(const Point& head, const Point& neck) {
  if (head.x == neck.x + 1) return Dir::Right;
  if (head.x == neck.x - 1) return Dir::Left;
  if (head.y == neck.y + 1) return Dir::Up;
  if (head.y == neck.y - 1) return Dir::Down;
  return Dir::Up;
}

const char* elimCauseToString(ElimCause c) {
  switch (c) {
    case ElimCause::NotEliminated: return "not-eliminated";
    case ElimCause::Starved: return "starved";
    case ElimCause::OutOfBounds: return "out-of-bounds";
    case ElimCause::SelfCollision: return "self-collision";
    case ElimCause::Collision: return "collision";
    case ElimCause::HeadToHead: return "head-to-head";
    case ElimCause::Hazard: return "hazard";
  }
  return "unknown";
}

int GameState::aliveCount() const {
  int n = 0;
  for (const auto& s : snakes)
    if (s.alive) ++n;
  return n;
}

int GameState::indexOf(const std::string& id) const {
  for (int i = 0; i < static_cast<int>(snakes.size()); ++i)
    if (snakes[i].id == id) return i;
  return -1;
}

bool GameState::isFoodAt(const Point& p) const {
  for (const auto& f : food)
    if (f == p) return true;
  return false;
}

bool GameState::isHazardAt(const Point& p) const {
  for (const auto& h : hazards)
    if (h == p) return true;
  return false;
}

bool GameState::isBodyAt(const Point& p) const {
  for (const auto& s : snakes) {
    if (!s.alive) continue;
    for (const auto& b : s.body)
      if (b == p) return true;
  }
  return false;
}

std::vector<Point> blockedAfterMove(const GameState& g) {
  std::vector<Point> out;
  for (const auto& s : g.snakes) {
    if (!s.alive) continue;
    // body[0 .. len-2] stays occupied; the tail (last element) vacates
    // unless duplicated (snake ate on the previous turn).
    for (size_t i = 0; i + 1 < s.body.size(); ++i)
      out.push_back(s.body[i]);
  }
  return out;
}

namespace {

// Head of s is inside other's body excluding its head cell (index 0).
bool hasBodyCollided(const SnakeState& s, const SnakeState& other) {
  for (size_t i = 1; i < other.body.size(); ++i)
    if (s.head() == other.body[i]) return true;
  return false;
}

// Head-to-head per standard.go: same head cell and len(s) <= len(other).
bool hasLostHeadToHead(const SnakeState& s, const SnakeState& other) {
  return s.head() == other.head() && s.length() <= other.length();
}

void placeFoodRandomly(GameState& g, int count, std::mt19937& rng) {
  std::vector<Point> candidates;
  candidates.reserve(static_cast<size_t>(g.width) * g.height);
  for (int y = 0; y < g.height; ++y) {
    for (int x = 0; x < g.width; ++x) {
      Point p{x, y};
      if (g.isFoodAt(p) || g.isBodyAt(p)) continue;
      candidates.push_back(p);  // hazards are allowed to host food
    }
  }
  if (candidates.empty()) return;
  for (int k = 0; k < count; ++k) {
    size_t idx = rng() % candidates.size();
    g.food.push_back(candidates[idx]);
    candidates.erase(candidates.begin() + static_cast<long>(idx));
    if (candidates.empty()) break;
  }
}

}  // namespace

TurnOutcome applyTurn(GameState& g, const std::vector<Dir>& moves, std::mt19937& rng) {
  TurnOutcome out;
  const int n = static_cast<int>(g.snakes.size());

  auto eliminate = [&](int i, ElimCause cause, const std::string& by) {
    SnakeState& s = g.snakes[i];
    if (!s.alive) return;
    s.alive = false;
    s.cause = cause;
    s.eliminatedBy = by;
    out.eliminatedIdx.push_back(i);
  };

  // 1. Movement: push new head, pop tail (always - growth is appended in the
  //    feeding stage, so the vacated tail cell is never re-occupied this turn).
  for (int i = 0; i < n; ++i) {
    SnakeState& s = g.snakes[i];
    if (!s.alive) continue;
    Dir d = (i < static_cast<int>(moves.size())) ? moves[i] : Dir::Up;
    Point nh = step(s.head(), d);
    s.body.insert(s.body.begin(), nh);
    s.body.pop_back();
  }

  // 2. Starvation stage: every alive snake loses 1 health.
  for (auto& s : g.snakes)
    if (s.alive) s.health -= 1;

  // 3. Hazard damage: applied only when the head is on a hazard with no food
  //    on the same cell. A hazard kill eliminates immediately and skips feeding.
  if (g.hazardDamagePerTurn > 0) {
    for (int i = 0; i < n; ++i) {
      SnakeState& s = g.snakes[i];
      if (!s.alive) continue;
      if (g.isHazardAt(s.head()) && !g.isFoodAt(s.head())) {
        s.health -= g.hazardDamagePerTurn;
        if (s.health < 0) s.health = 0;
        if (s.health <= 0) eliminate(i, ElimCause::Hazard, "");
      }
    }
  }

  // 4. Feeding: every snake whose head is on food eats it (health reset to
  //    max, duplicate tail appended, food removed once eaten by anyone).
  {
    std::vector<Point> remaining;
    remaining.reserve(g.food.size());
    for (const auto& f : g.food) {
      bool eaten = false;
      for (auto& s : g.snakes) {
        if (!s.alive || s.body.empty()) continue;
        if (s.head() == f) {
          s.body.push_back(s.body.back());  // growSnake: duplicate current tail
          s.health = 100;
          eaten = true;
        }
      }
      if (!eaten) remaining.push_back(f);
    }
    g.food = std::move(remaining);
  }

  // 5a. Elimination, phase one: health and out-of-bounds. These bodies stop
  //     blocking immediately (standard.go marks them before collision checks).
  for (int i = 0; i < n; ++i) {
    SnakeState& s = g.snakes[i];
    if (!s.alive) continue;
    if (s.health <= 0) { eliminate(i, ElimCause::Starved, ""); continue; }
    if (!g.inBounds(s.head())) { eliminate(i, ElimCause::OutOfBounds, ""); continue; }
  }

  // 5b. Phase two: collisions collected among survivors, applied afterwards,
  //     so mutually colliding snakes still eliminate each other.
  {
    struct Pending { int idx; ElimCause cause; std::string by; };
    std::vector<Pending> pending;
    for (int i = 0; i < n; ++i) {
      SnakeState& s = g.snakes[i];
      if (!s.alive) continue;

      // Self-collision first.
      if (hasBodyCollided(s, s)) {
        pending.push_back({i, ElimCause::SelfCollision, s.id});
        continue;
      }
      // Body collision with other survivors second.
      bool bodyHit = false;
      for (int j = 0; j < n && !bodyHit; ++j) {
        if (j == i) continue;
        const SnakeState& other = g.snakes[j];
        if (!other.alive) continue;
        if (hasBodyCollided(s, other)) {
          pending.push_back({i, ElimCause::Collision, other.id});
          bodyHit = true;
        }
      }
      if (bodyHit) continue;
      // Head-to-head last.
      for (int j = 0; j < n; ++j) {
        if (j == i) continue;
        const SnakeState& other = g.snakes[j];
        if (!other.alive) continue;
        if (hasLostHeadToHead(s, other)) {
          pending.push_back({i, ElimCause::HeadToHead, other.id});
          break;
        }
      }
    }
    for (const auto& p : pending)
      eliminate(p.idx, p.cause, p.by);
  }

  // 6. Game over when at most one snake remains.
  out.gameOver = g.aliveCount() <= 1;

  // 7. Food spawn (map logic): refill to the minimum, else roll the chance.
  if (g.foodSpawning && !out.gameOver) {
    int deficit = g.minimumFood - static_cast<int>(g.food.size());
    if (deficit > 0) {
      placeFoodRandomly(g, deficit, rng);
    } else if (g.foodSpawnChance > 0 &&
               static_cast<int>(rng() % 100) < g.foodSpawnChance) {
      placeFoodRandomly(g, 1, rng);
    }
  }

  ++g.turn;
  return out;
}

}  // namespace bs
