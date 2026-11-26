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

**Note the `@` before the path** — it tells curl to read the request *body*
from that file (`-d @FILE`). Without it, curl sends the literal text
`tests/fixtures/move_request_duel.json` as the body, the server's JSON
parser rejects it, and you get the fallback answer plus a
`parse_error.101 ... last read: 'te'` line in the server log. The path is
relative to your current directory: run the command from inside
`battlesnake-cpp/`, or use an absolute path after the `@`.

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
  are still applied **simultaneously** through the turn engine, so
  head-to-head, tail-vacating and two-phase eliminations inside the search
  are exact.
- **Determinism**: the search never spawns food, since future food spawns
  are random; everything else (hazards, growth, starve, H2H) is simulated
  exactly.
- **v3 — bitboard hot path** (`src/board.hpp`, `src/fastboard.cpp`): the
  search no longer copies `GameState`s per child. `FastState` keeps each
  body in a ring buffer (a move writes the new head one slot before the
  current one; the duplicated-tail growth model falls out of the ring for
  free — feeding is a length increment plus one cell write) and all cells
  as multiword bitsets, so `makeTurnFast`/`unmakeTurnFast` apply/undo a
  whole turn with a handful of word operations and a ~700-byte undo
  snapshot. Flood fill and Voronoi territory are layered bitboard BFS
  (frontier shifts instead of per-cell loops). Equivalence to the rules
  engine is enforced by `tests/test_fastboard.cpp`: 3000 randomized
  differential cases replayed through both `applyTurn` and
  `makeTurnFast` (post-turn states, unmake-restore, remake determinism)
  plus 800 flood/Voronoi cases against naive per-cell BFS. Measured:
  **+0.5–0.8 turns of depth at the same budget** (4.6→5.4 avg in duels at
  25 ms; 7.7 at 350 ms) and the snek-two match jumped from 84–88% to
  **94%**.
- **Alpha-beta pruning** with move ordering: children sorted by their static
  evaluation (enemy replies worst-for-me first), previous iteration's best
  move searched first at the root.
- **v4a — Stockfish-style pruning layer** (on top of the bitboard speedup;
  measured alone it bought +0.2–0.3 turns):
  * **Aspiration windows**: each iterative-deepening iteration searches
    around the previous value (±150 eval units) instead of (−∞, +∞); a fail
    low/high triggers one full-window re-search.
  * **Late-move reductions**: late children in a min node are searched one
    turn shallower; a value that would still lower the minimum triggers a
    full-depth re-search (the "trust but verify" pattern).
  * **Futility pruning**: near the horizon, children statically ≥ β+150
    cannot become the minimum and keep their prior as value.
  * **Killer replies**: the joint enemy reply that refuted one of my moves
    at a depth is tried first against my sibling moves at the same depth.
  * **Root move ordering**: root moves are ordered by their previous
    iteration's value, not just best-first.
- **v4b — endgame cutoff logic** (duel-only): under clear territory
  dominance (my Voronoi > enemy + 8), the enemy's own flood space is
  tracked; positions where the enemy's space < its length get a large
  direct bonus ("sever the region: the enemy is dying"), otherwise the
  squeeze continues at double territory slope.
- **v5 — Zobrist-keyed transposition table** (`src/tt.hpp`, maintained in
  `src/fastboard.cpp`): every position gets a 64-bit identity hash keyed on
  the **full body sequences** (a rolling polynomial hash, updated in O(1)
  per move: prepend the head, drop the tail), healths, food, turn, board
  shape, viewer and hazard damage — the key is exact (the from-scratch
  derivation is checked after every turn of the 3000-case differential
  fuzz) and deliberately sequence-sensitive, because the vacating order of
  body cells determines the future. The 2^16-entry table caches two things:
  search bounds (exact/lower/upper + remaining depth + best move, reused
  across iterative-deepening iterations and across game turns — one real
  turn shifts stored positions one ply down the tree, so along the played
  line the depths line up exactly) and **static leaf evaluations** (the
  eval dominates node cost and iterative deepening re-expands interior
  nodes every iteration, so the eval cache is where the TT pays off most:
  thousands of hits per 25 ms search). Soundness: only verified values are
  stored — futility shortcuts and *unverified* LMR cutoffs taint their
  nodes (reading is always sound, approximate values are never written;
  storing unverified LMR cutoffs was measured to cost 10 points against
  snek-two, 94%→84%, exactly because endgames reuse the table the most).
  `tests/test_tt.cpp` proves value preservation: strict alpha-beta with and
  without the TT returns identical root values on 123 position-depths.
  Measured: depth 7 (was 6) on mid-game positions at 25 ms, depth 9 (was
  8) at 100 ms, thousands of cache hits per search.
- **v6 — tail-aware flood fill** (port of hovering-hobbs
  `spread_from_head_tail_aware`): every fill in the eval now models the
  **vacate schedule** of body cells — the square at ring index i (head =
  0) opens on cycle `len − i`, taking the head-ward-most occurrence for
  stacked squares (a just-fed double tail keeps its extra tick).
  `blockedAtCycle(c) = occ & ~open` is O(1) multiword ops per cycle, no
  per-cell arrays. The multi-source fill (territory, food control,
  dominance) runs all alive heads one ring per cycle, longest snake
  first — the longer snake wins same-cycle contests — with cycle cap 12
  (the original's tuned budget); a frontier with no enterable neighbour
  dies (the fill models paths that move every tick, which is what a
  snake can actually do). The single-source space/tail fill runs to
  frontier exhaustion (unbounded-equivalent cap). Net effect: territory,
  cycle room and tail reachability now count space **through vacating
  tails** — a snake coiled against itself survives by chasing its tail,
  which the static fill scored as sealed (that was the endgame gap
  against hobbs). The food clock stays deliberately static (bodies
  blocked — the original's Dijkstra pessimism: a path that only exists
  behind a vacating tail is not food to be counted on). Ground truth:
  the original's hand-derived fixtures are ported in
  `tests/test_tailaware.cpp` (sealed corner → 19 squares in 5 cycles,
  stacked-tail frontier death, open-on-schedule 4/4/5, serpentine walls
  stay) plus a 400-state fuzz checking prefix monotonicity in the cap
  and entered-set disjointness. Self-play (equal 50 ms budgets, HTTP,
  80 duels/direction): new eval beats the v5 eval **85–15** (games
  average ~409 turns — the long endgames this change targets). External
  benchmarks moved too: **snek-two 98% (49–1)**, up from 94%, and the
  hovering-hobbs match flipped from 0–4 to **3–1 / 2–2** at the 100 ms
  harness budget.
- **Iterative deepening with a hard deadline** checked at *every* node:
  depth 1, 2, 3, ... until time runs out; incomplete iterations are
  discarded (the last fully completed depth decides); a proven forced win
  stops the search early; if nothing completed, the v1 move is used.
  Typical depth: 5.2–5.4 in duels at 25 ms (early-game positions are
  flood-dominated; the TT gains show more in mid/late game), up to 16 on
  forced lines.
- **Leaf evaluation** (weighted sum, tuned against the 2018 champion — see
  the benchmark below): tail-aware territory (8 per cell; cells claimed by
  me through the vacate-schedule fill, vs the strongest enemy's claim),
  **cycle room** (tail-aware reachable space minus length minus a growth
  margin, penalized steeply so the snake never eats itself into a sealed
  corner; the wake behind the own tail counts as space), **own-tail
  reachability** (the fill actually walks onto the vacating tail — the
  endgame survival signal), **food control** (+ for food inside my
  territory including behind vacating tails, − for food feeding the
  enemy), the food clock (static-BFS distance to food vs health; urgency
  grows as health drains), length differential, mobility, head-to-head
  pressure.
- **Safety inheritance**: root moves come only from the v1 legal filter; if
  exactly one legal move exists the search is skipped entirely; any failure
  falls back to v1. v2 can never return a move v1 would reject.

### Project layout

```
src/rules.hpp/.cpp      rules engine (forward model, applyTurn)
src/board.hpp           bitboard primitives (multiword bitsets, shifts, BFS)
src/fastboard.hpp/.cpp  FastState: bitboard game state, makeTurnFast/unmake,
                         Zobrist key maintenance (rolling sequence hash)
src/strategy.hpp/.cpp   v1 survival core (evaluateMoves, chooseMoveV1)
src/search.hpp/.cpp     v2-v5 paranoid minimax over FastState (searchV2)
src/tt.hpp/.cpp         transposition table: search bounds + static-eval cache
src/main.cpp            HTTP server: GET /, POST /start, /move, /end
tests/test_rules.cpp    50 unit tests: every rules edge case + strategy filter
tests/test_fastboard.cpp  differential proof: 3000 makeTurn/unmake cases vs
                         applyTurn + 800 flood/Voronoi cases vs naive BFS +
                         Zobrist key == from-scratch derivation on every turn
tests/test_tt.cpp       TT soundness: strict alpha-beta with vs without the
                         TT returns identical values (123 position-depths)
tests/test_search.cpp   29 unit tests: trap/H2H/food/timeout/fuzzed legality
tests/test_tailaware.cpp  tail-aware fill ground truth (hobbs fixtures) + fuzz
tests/selfplay.cpp      full-game harness: --a/--b baseline|v1|v2|snek2|http://…, budgets
tests/fixtures/         official example request + mid-game duel request
bench/snek_two_bridge.cpp  driver linking the cloned snek-two opponent
bench/snek_two_debug.cpp   single-position debug driver for the same opponent
bench/test_req_2018.json   2018-API sample request feeding the bridge drivers
bench_*_results.txt        raw v6 benchmark logs: old-vs-new self-play,
                           snek-two 98%, hobbs 3-1 / 2-2
solutions.md               solutions research + ranked roadmap
../opponents/snek-two/  the cloned 2018 champion (one-time setup, see above)
third_party/            nlohmann/json v3.11.3, cpp-httplib v0.18.3 (vendored)
Makefile                build + test + bench-snek2 targets
```

## 4. How to test

`make test` runs everything: **80 unit checks + tail-aware suite + 520 self-play games** (~10
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

# 3. Tail-aware flood fill ground truth (hobbs's hand-derived fixtures:
#    sealed-corner escape, stacked-tail death, open-on-schedule) + fuzz
./build/test_tailaware

# 4. Self-play harness (full games, starter-level baseline / v1 / v2)
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
| Duel, v6 (tail-aware) vs v5 eval, both orders (80+80) | v6 | **85% / 81%** — 68–12 and 65–15 (games ~400 turns: the endgames this change targets) |
| Duel, v2 vs snek-two (50 games) | v2 | **98%** — 49–1 (was 94% pre-tail-aware; avg 223 turns) |
| Duel, v2 vs hovering-hobbs (4, 100 ms) | even | **3–1** (seed 3141) and **2–2** (seed 2718) — was 0–4 before the tail-aware fill; the remaining losses are 360+ turn endgames |
| Duel, v2 vs POOOOOOOOOOOOOG (15) | v2 | **100%** — 15–0 (93.3% pre-TT) |
| Duel, v2 vs Untimely Neglected Wearable (15) | v2 | **93.3%** — 14–1 (pre-bitboard build) |
| Duel, v2 vs v1 (60 games) | v2 | **95%** — 57–3 |
| Duel, v2 vs baseline (60) | v2 | **95%** — 57–3 (losses: 3 starvation races) |
| FFA, v2 vs 3 baselines (60) | v2 | **95%** (random chance is 25%) |
| Duel, v1 vs baseline (200) | v1 | 89.5% (regression, unchanged by v2 work) |
| FFA, v1 vs 3 baselines (100) | v1 | 80.0% (regression, unchanged) |

Also verified: zero avoidable deaths across all suites for v1 and v2 alike;
the live server reaches depth 6 in ~350 ms on a mid-game duel request (0.35 s
round trip, 150 ms under the engine's 500 ms limit), honors both environment
knobs, and answers malformed input with a valid move.

Latency note: the deadline is checked at every search node, so CPU-bound
overrun is sub-millisecond. Occasional wall-clock overshoot (up to ~+20 ms
observed in a shared sandbox) comes from thread preemption, which the 150 ms
headroom absorbs.

### Benchmark: v2 vs snek-two, the 2018 Advanced Division winner

The opponent is a real external snake:
[aleksiy325/snek-two](https://github.com/aleksiy325/snek-two), winner of the
Battlesnake 2018 Advanced Division (C++, minimax + alpha-beta with a
genetically tuned Voronoi/arctan-food heuristic). It is cloned to
`../opponents/snek-two` and driven through `bench/snek_two_bridge`, a small
driver that links its `MinimaxSnake::decideMove` directly (no HTTP server
needed). Two fixes were required to run 2018 code against today's rules:

1. **Their `GameState(width, height)` constructor leaves `max_food`/`cur_food`
   uninitialized**, so `cleanup()`'s `while (cur_food < max_food) addFood();`
   spins forever on stack garbage (~30% of runs). Patched to zero (no food
   spawning inside their search) — no decision logic changed.
2. **The 2018 API grew `y` downward (screen coordinates); today's API grows
   `y` upward.** The bridge serializes our board with a vertical mirror
   (`y' = height-1-y`) so their geometry matches ours; move strings then mean
   the same visual direction in both frames. Without it their snake walks
   into the top wall believing it is heading into open space.

```
make bench-snek2   # 50 duels, v2 (25 ms) vs snek-two (full strength, ~4 ms)
```

Results (duel, 50 games unless noted): v2 wins **98% (49–1)** at the 25 ms
harness budget after the v6 tail-aware fill (was 94%/47–3 on the bitboard
build) with games running ~223 turns on average. The remaining loss is a
long endgame territory war (enclosure with zero legal moves). Raw logs:
`bench/bench_snek2_v6_results.txt`.

The eval was tuned on these games: territory weight 6→8 (snek-two's GA
settled at 7.6), the steep cycle-room slope (v2 previously ate food that
spawned inside its own sealed region, grew, and self-enclosed), own-tail
reachability, and food control. That tuning took the match from ~86–90% to
92% while keeping every local suite above its threshold; the trade-off is a
slightly more territorial (less food-greedy) v2, which shows as 90% instead
of 100% in the FFA-vs-baselines suite.

### Benchmark: v2 vs the snake-zoo (modern opponents)

Three more real opponents from
[BattlesnakeOfficial/snake-zoo](https://github.com/BattlesnakeOfficial/snake-zoo),
all driven through the harness's generic HTTP strategy (`--b
http://localhost:PORT/move`, current API serialization, previous-move
repetition on failure — exactly the real engine's behavior):

| Opponent | What it is | How to run locally |
|---|---|---|
| [POOOOOOOOOOOOOG](https://github.com/calvinl4/battlesnake-winter-2022) | JS minimax + α-β + A*, Gold rank Spring 2021 | `cd ../opponents/battlesnake-winter-2022 && npm i && PORT=8423 node index.js` |
| [Untimely Neglected Wearable](https://github.com/altersaddle/untimely-neglected-wearable) | Python rule-based + flood fill, 308+ commits | `cd ../opponents/untimely-neglected-wearable && pip install -r requirements.txt && PORT=8424 python server.py` |
| [Hovering Hobbs](https://github.com/coreyja/battlesnake-rs) | Rust paranoid minimax + tail-aware flood fill, top of today's leaderboard | see below |

Legacy-API note: snakes from the older API era expect a `head` field on
every snake object (the harness includes it; modern snakes ignore extras).

**Hovering Hobbs** deserves its own note. The upstream `web-axum` server
needs `protoc` for its OTLP telemetry, so `opponents/battlesnake-rs/
battlesnake-rs/examples/hobbs_server.rs` (added by us) is a ~130-line
standalone HTTP server that calls the exact same `ParanoidMinimaxSnake`
entry points as upstream's route handler, minus incremental tree reuse:

```
cd ../opponents/battlesnake-rs && cargo build --release --example hobbs_server
PORT=8422 ./target/release/examples/hobbs_server
./build/selfplay --games 4 --mode duel --a v2 \
    --b http://localhost:8422/move --v2-budget-ms 100
```

It is the strongest opponent in the zoo, and honestly the strongest in the
whole benchmark: it spends its full ~380 ms per move (500 ms timeout minus
150 ms latency padding) and grinds games to 360+ turns. Before the v6
tail-aware fill it beat our v2 **3–1** (5.8 / 6.5 average search depth)
and **4–0** after the v5 transposition table — all our losses were long
endgame enclosures, which is precisely the scenario a vacate-schedule fill
scores correctly. After v6 the match flipped: **3–1** (seed 3141) and
**2–2** (seed 2718) at the same 100 ms harness budget (v2 depth 7.3–7.4;
raw logs `bench/bench_hobbs_v6_results.txt`,
`bench/bench_hobbs_v6b_results.txt`). One hobbs crash occurred on a
triple-stacked body fed by the harness (upstream's converter rejects
3-segments-on-one-square stacks with a panic; the game counted as our win
— without it the seed-3141 set is a clean 2–1). Their remaining edge is
architectural: *incremental tree reuse* across turns (the real engine
keeps last turn's minimax tree and replays the opponent's actual moves
into it; our standalone example measures the plain re-search) plus a ~4×
compute budget advantage over the harness's 100 ms cap. The remaining
work against it: true incremental tree reuse between turns, parallel
lazy-SMP search, and endgame-specific eval. Treat 98% vs snek-two and the
other zoo results as "solidly above the field"; hobbs is the honest
frontier.

## 5. Deploying

Any always-on host works: run the binary behind a reverse proxy or tunnel,
then register the HTTPS URL at arena.battlesnake.com. Avoid serverless cold
starts — a late response makes the engine repeat your previous move, even
into a wall. Keep `BATTLENSNAKE_BUDGET_MS` at 350 or lower so network plus
compute stays inside the 500 ms budget.

## 6. Roadmap

- **Done (v3 bitboards):** incremental make/unmake on ring-buffer bodies +
  bitset boards, layered bitboard flood/Voronoi. The node-representation
  half of the hobbs gap; Lazy SMP stays postponed until a multi-core host
  is available (this deployment is single-core).
- **Done (v5 transposition table):** Zobrist sequence-hash keys with O(1)
  incremental maintenance, 2^16-entry table, eval cache + verified search
  bounds, strict-mode value-preservation tests. Measured: +1 depth on
  mid-game benchmarks at equal wall-clock (7 vs 6 at 25 ms), thousands of
  cache hits per search, POG 93→100% — but **no** rating change against
  hobbs: position snapshots don't replace their incremental tree, and the
  harness caps us at 100 ms vs their 380 ms.
- **Next (the rest of the hobbs gap):** true incremental tree reuse between
  turns (keep the searched tree, replay the actual game moves into it,
  extend the frontier — strictly stronger than TT snapshots), parallel
  lazy-SMP search, and running our full 350 ms budget instead of the
  harness's 25 ms.
- **v3 (other):** per-ladder specialization (Duels depth, Royale hazard
  planning, Constrictor space-only evaluation), replay-driven weight tuning.
- **v4 (optional):** RL hybrid — PPO self-play policy with the legal filter
  as the safety supervisor.
