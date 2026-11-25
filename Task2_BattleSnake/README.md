# Battlesnake C++ — v2 Minimax Search Engine

A [Battlesnake](https://arena.battlesnake.com) server in C++17 with no external
dependencies (cpp-httplib and nlohmann/json are vendored under `third_party/`,
built with a plain Makefile).

The move decision is a **paranoid minimax with alpha-beta pruning and
iterative deepening** running on top of a rules engine that is an exact port
of the official game rules. A simpler "survival core" (v1) remains in the
codebase as the search's safety net and fallback.

> This README is the project documentation

---

## 1. Quick start

Requirements: any C++17 compiler (`g++` or `clang++`) and `make`. Nothing else.

```bash
make                      # -> build/battlesnake (the HTTP server)
PORT=8080 ./build/battlesnake
```

The server prints `Battlesnake v2 (search, budget 350ms) listening on
0.0.0.0:8080` and answers on four endpoints. That is all that is needed to
play: register the publicly reachable URL at arena.battlesnake.com (see
"Deploying" below), or point the local CLI/any client at `localhost:8080`.

## 2. Running the server

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PORT` | `8080` | HTTP listen port |
| `BATTLENSNAKE_BUDGET_MS` | `350` | Search time budget per move, in milliseconds. The engine allows 500 ms; 350 leaves ~150 ms of headroom for network and scheduling |
| `BATTLENSNAKE_STRATEGY` | `v2` | `v2` = full minimax search; `v1` = serve the fast survival-only engine (~0.1 ms per move) |

### Endpoints

- `GET /` — engine metadata (JSON, required by the Battlesnake engine)
- `POST /start` — game started (no-op)
- `POST /move` — the decision endpoint: request JSON in, `{"move": "...", "shout": "..."}` out
- `POST /end` — game ended (no-op)

Every `/move` call is logged to stderr with game id, turn, chosen move,
reached search depth and compute time, e.g.:

```
[move] game=duel-test turn=42 move=up depth=6 compute=350.081ms
```

### Try it with curl

An example request is included at `tests/fixtures/move_request.json` (the
official example position, where "up" is the only legal move — the engine
answers it via its forced-move shortcut in ~0.07 ms) and
`tests/fixtures/move_request_duel.json` (a real mid-game duel position that
exercises the full search, depth 6 at the default 350 ms budget):

```bash
curl -s -w "\n[%{time_total}s]\n" -X POST localhost:8080/move \
     -H "Content-Type: application/json" \
     -d @tests/fixtures/move_request_duel.json
```

Malformed or invalid input still returns a valid move (`{"move":"up",...}`)
— an invalid response makes the real engine repeat your previous move, even
into a wall, so every failure path answers.

## 3. How it all works

### Request flow

```
POST /move (JSON)
  -> stateFromJson: board/food/hazards/snakes -> GameState (ruleset-aware:
     food spawn chance, minimum food, hazard damage, Constrictor detection)
  -> searchV2(g, your-snake-id, budget)
       -> paranoid minimax + alpha-beta + iterative deepening
       -> any failure falls back to chooseMoveV1
  -> {"move": "up", "shout": "v2 minimax"}
```

The decision is a pure function of the request state: no per-game memory, no
shared mutable state, so the server is safe under concurrent games.

### The board and state model (`src/rules.hpp`)

- `Point {x, y}` — origin at the **bottom-left** (`y` grows upward), matching
  the Battlesnake coordinate system.
- `SnakeState {id, body, health, alive, cause}` — `body[0]` is the head,
  `body.back()` the tail; `length()` is `body.size()`.
- `GameState {width, height, turn, food, hazards, snakes, foodSpawnChance,
  minimumFood, hazardDamagePerTurn, foodSpawning}` plus helpers
  (`inBounds`, `isBodyAt`, `isFoodAt`, `aliveCount`, `indexOf`).
- `step(p, dir)` / `dirFromNeck(...)` handle movement; `Dir::Up/Down/Left/Right`
  are the four moves.

### The rules engine (`src/rules.cpp`, `applyTurn`)

A faithful port of [BattlesnakeOfficial/rules](https://github.com/BattlesnakeOfficial/rules)
`standard.go`, including the exact stage order:

1. move every alive snake one cell (simultaneously),
2. health −1 per turn,
3. hazard damage (Royale: 14/turn unless food sits on the cell),
4. feeding (health reset + **duplicated tail growth** — a snake that ate does
   *not* pop its tail this turn),
5. two-phase elimination: starvation/out-of-bounds/self-collision first,
   then head-to-head (`len(self) <= len(other)` dies; equal lengths kill
   both) and body collisions — with starved snakes already un-blocked.

`blockedAfterMove(g)` returns the exact set of cells that will still be
occupied after the simultaneous move: `body[0..len-2]` of every alive snake.
Because tails pop (unless the snake ate), a tail cell is enterable — the
engine knows this, which is a large real-game advantage over naive bots.

The only approximation in the whole project is *future* food spawning
(map logic, random): refill to the minimum, else a 15% roll. It is used only
by the local self-play harness — real engine requests always contain the
current food.

### v1 survival core (`src/strategy.cpp`, `chooseMoveV1`) — the fallback layer

1. **Legal-move filter**: every candidate move must be in bounds and land on
   a cell from `blockedAfterMove` — with this filter a snake can never die by
   wall, self-collision or body collision while any alternative exists.
2. **Enclosure guard**: flood fill from each candidate head, *pessimistic*
   (cells an enemy head can reach next turn don't count as escape room);
   space below own length is heavily penalized.
3. **Head-to-head avoidance**: never step adjacent to an equal-or-longer
   enemy head when a safe move exists.
4. **Food clock**: BFS distance to nearest reachable food, urgency ramps up
   as `health - distance` shrinks.
5. **Tail-chase fallback** and hazard awareness.

~0.1 ms per move; it wins 89.5% of duels against a starter-level baseline.

### v2 search engine (`src/search.cpp`, `searchV2`)

- **Paranoid minimax, one game turn per tree level.** Within a level, my move
  is chosen first (max node), then every alive enemy's reply is enumerated as
  a Cartesian product (min node) — enemies may counter my exact move, so the
  search value is a sound *lower bound* on my position. All moves of a turn
  are still applied **simultaneously** through `applyTurn`, so head-to-head,
  tail-vacating and two-phase eliminations inside the search are exact.
- **Determinism**: search-state copies set `foodSpawning = false`, since
  future food spawns are random; everything else (hazards, growth, starve,
  H2H) is simulated exactly.
- **Alpha-beta pruning** with move ordering: children sorted by their static
  evaluation (enemy replies worst-for-me first), previous iteration's best
  move searched first at the root.
- **Iterative deepening with a hard deadline** checked at *every* node:
  depth 1, 2, 3, ... until time runs out; incomplete iterations are
  discarded (the last fully completed depth decides); a proven forced win
  stops the search early; if nothing completed, the v1 move is used.
  Typical depth: 6 in duels at 350 ms, 3–4 in 4-snake FFA, up to 16 on
  forced lines.
- **Leaf evaluation** (weighted sum): Voronoi territory (cells whose nearest
  head is mine only, vs the strongest enemy's), reachable space vs own
  length (enclosure penalty), the food clock (distance to food vs health,
  starvation penalty), length differential, mobility, head-to-head pressure.
- **Safety inheritance**: root moves come only from the v1 legal filter; if
  exactly one legal move exists the search is skipped entirely; any failure
  falls back to v1. v2 can never return a move v1 would reject.

### Project layout

```
src/rules.hpp/.cpp      rules engine (forward model, applyTurn)
src/strategy.hpp/.cpp   v1 survival core (evaluateMoves, chooseMoveV1)
src/search.hpp/.cpp     v2 paranoid minimax + iterative deepening (searchV2)
src/main.cpp            HTTP server: GET /, POST /start, /move, /end
tests/test_rules.cpp    50 unit tests: every rules edge case + strategy filter
tests/test_search.cpp   29 unit tests: trap/H2H/food/timeout/fuzzed legality
tests/selfplay.cpp      full-game harness: --a/--b baseline|v1|v2, budgets
tests/fixtures/         official example request + mid-game duel request
third_party/            nlohmann/json v3.11.3, cpp-httplib v0.18.3 (vendored)
Makefile                build + test targets
```

## 4. How to test

`make test` runs everything: **79 unit tests + 520 self-play games** (~10
minutes, because the search suites burn their time budgets in every move).
Each piece can also be run on its own:

```bash
make tests                # build the three test binaries

# 1. Rules + v1 strategy unit tests (50 checks)
./build/test_rules

# 2. Search unit tests (29 checks)
./build/test_search
#   covers: forced-move shortcut, food seeking when starving, avoidance of
#   a sealed pocket trap, never entering a losing head-to-head, safe
#   behavior with a microscopic budget, no-crash on doomed positions,
#   search-depth sanity, and a fuzzed legality sweep over 60 random states.

# 3. Self-play harness (full games, starter-level baseline / v1 / v2)
./build/selfplay --games 200 --mode duel --a v1 --b baseline
./build/selfplay --games 100 --mode duel --a v2 --b v1 --v2-budget-ms 25
./build/selfplay --games 60  --mode duel --a v2 --b baseline --v2-budget-ms 25
./build/selfplay --games 100 --mode ffa  --a v1 --b baseline
./build/selfplay --games 60  --mode ffa  --a v2 --b baseline --v2-budget-ms 25
#   --a = hero (tracked for stats and safety assertions)
#   --b = every opponent's strategy; --mode duel (1v1) or ffa (1 v 3)
#   --v2-budget-ms caps the search per move (keep small in the harness)

# 4. Sanitizers (memory safety on the same suites)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -pthread \
    -Ithird_party -Isrc tests/test_search.cpp \
    src/rules.cpp src/strategy.cpp src/search.cpp -o build/test_search_asan
./build/test_search_asan
```

The harness is a hard test, not a demo: it fails if the hero ever dies by
wall, self-collision or body collision **while a legal move existed** (that
would be a safety-filter bug), and it enforces win-rate thresholds. Deaths
with zero legal moves (fully enclosed) are reported separately as honest
losses.

### Measured results (this machine, 25 ms harness budget)

| Suite | Winner | Result |
|---|---|---|
| Duel, v2 vs v1 (100 games) | v2 | **99%** (v1's one win: a food race) |
| Duel, v2 vs baseline (60) | v2 | **100%** |
| FFA, v2 vs 3 baselines (60) | v2 | **100%** (random chance is 25%) |
| Duel, v1 vs baseline (200) | v1 | 89.5% (regression, unchanged by v2 work) |
| FFA, v1 vs 3 baselines (100) | v1 | 80.0% (regression, unchanged) |

Also verified: ASan + UBSan clean on both test binaries; zero avoidable
deaths across all suites for v1 and v2 alike; the live server reaches depth
6 in ~350 ms on a mid-game duel request (0.35 s round trip, 150 ms under the
engine's 500 ms limit), honors both environment knobs, and answers malformed
input with a valid move.

Latency note: the deadline is checked at every search node, so CPU-bound
overrun is sub-millisecond. Occasional wall-clock overshoot (up to ~+20 ms
observed in a shared sandbox) comes from thread preemption, which the 150 ms
headroom absorbs.

## 5. Deploying

Any always-on host works: run the binary behind a reverse proxy or tunnel,
then register the HTTPS URL at arena.battlesnake.com. Avoid serverless cold
starts — a late response makes the engine repeat your previous move, even
into a wall. Keep `BATTLENSNAKE_BUDGET_MS` at 350 or lower so network plus
compute stays inside the 500 ms budget.

## 6. Roadmap

- **v3:** per-ladder specialization (Duels depth, Royale hazard planning,
  Constrictor space-only evaluation), transposition table, replay-driven
  weight tuning (the one observed v2 loss was a starvation race — a
  food-clock weight question, not a safety issue).
- **v4 (optional):** RL hybrid — PPO self-play policy with the legal filter
  as the safety supervisor.
