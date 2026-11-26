# Solutions research: hobbs repo deep-dive + external game-engine literature

Date: 2026-11-26. Sources: coreyja/battlesnake-rs (hovering-hobbs), read from upstream
GitHub (the local clone was emptied; the built `hobbs_server` binary is what our
benchmarks ran against), plus the articles listed in part 2.

## Part 1 — What hovering-hobbs actually does (and what we assumed wrongly)

**Correction of our diagnosis:** their "incremental tree reuse" is **ordering-only**.
They keep the entire searched tree per game (`web-axum/src/hobbs.rs:19-43`), replay the
snakes' *actual* moves into it by head-diff, descend the PV, and use the reached subtree
**only as a best-first move-ordering seed** for this turn's search
(`battlesnake-minimax/src/paranoid/move_ordering.rs:25-44`). No values or bounds are
reused; iterative deepening restarts at depth 1 every turn; **they have no TT, no
aspiration, no LMR, no killers, no futility**. Our search machinery is strictly richer.
Their advantage is **evaluation quality + time**, not search tech.

Their structure, confirmed:

- **Paranoid alpha-beta with one ply per SNAKE** (not per turn): mover =
  `players[depth % players.len()]`, we move first; simultaneous moves are accumulated
  and jointly applied when all living snakes have moved; leaves only at full-turn
  boundaries (`eval.rs:452,418-433`; `paranoid/score.rs:107-109`).
- **Eval = `standard_score_tail_aware`** (the big one):
  - Tail-aware simultaneous multi-source flood fill: all heads spread one ring per
    cycle; body cell at index i from the head becomes enterable on cycle `len - i`;
    a blocked frontier dies; the double-stacked (just-fed) tail takes one extra tick
    (`flood_fill/spread_from_head_tail_aware.rs:87-155`). Contested squares go to the
    **longest** snake (snakes processed longest-first per cycle).
  - Square weights: food=20, hazard=1, empty=5; score is the **ratio** my_weighted /
    total_weighted (share of the board) + length term (`hovering_hobbs.rs:23-27,238-241`).
  - Length term: `(my_len - longest LIVING rival)` clamped ±3, weight 0.16 of the ratio.
  - **Lexicographic health mode**: below health 60 (duel) / 85 (2+ rivals) the score
    switches to `LowOnHealth(-A*dist_to_nearest_food, ratio)` which sorts *below* every
    normal score — a hard cliff, not a weighted urgency (`:15-19,48,71`). Thresholds
    tuned by controlled self-play with z-scores.
  - 12-cycle cap, early exit when a frontier dies; A*/Dijkstra with +1 food/hazard step
    penalties for the food distance.
- **Terminal preferences**: `Win(Reverse(depth))` (win sooner), `Lose(Reverse(alive_count),
  depth)` — die as late as possible **and take as many rivals down as possible** (mutual
  destruction ranks above solo death) (`paranoid/score.rs:13-31,113-131`). Proven-terminal
  root score stops the iteration early.
- **Time**: budget = timeout − 150 ms fixed latency padding = ~350 ms; worker thread
  searches, main thread busy-polls until budget and halts the worker between move
  expansions; last fully completed depth wins.
- Not present: opening book, symmetry, opponent modeling, food-spawn simulation,
  starvation logic beyond the health cliff.

### Ranked deltas worth porting to our engine

1. **Tail-aware flood fill** (highest value): our Voronoi/cycle-room treats all body
   cells as blocked for the whole search; theirs vacates cell i after `len-i` cycles,
   resolves contested cells longest-first, and models the just-fed tail's extra tick.
   This is exactly the eval that wins 400-turn endgames — space that *looks* sealed to
   us is winnable to them (following the tail), and space they *show* dying is really
   dying. 2nd source: Schier 2019 (part 2) uses the identical longest-first sorted-queue
   flood fill.
2. **Ratio-based weighted territory**: food cells worth 20, empty 5, hazards 1; score =
   my share / total share. Flat Voronoi cell counts undervalue food-rich regions and
   overvalue hazard-adjacent junk space (royale).
3. **Lexicographic health cliff** below 60/85: currently our food-clock urgency is a
   weighted term — a deep search can still trade health for territory; theirs cannot.
   Cheap to add as a score-category switch.
4. **Length-vs-longest-living-rival**, clamped ±3 at ~0.16 of the territory ratio:
   bounded length greed that avoids suicidal food races while still preferring growth
   in otherwise-equal positions.
5. **Terminal ordering**: add "dying later" and "mutual destruction beats solo death"
   lexicographic terms to `terminalValueFast` (we only have win-early/lose-late via
   ±500·turn).
6. **Tree-replay ordering seed**: keep last turn's tree (or just the PV + refutation
   replies), replay actual moves by head-diff, seed move ordering. Cheaper than and
   complementary to our TT (which they don't have — they compensate with ordering).
7. **One ply per snake** instead of one joint-reply level per turn: finer alpha-beta
   cutoffs (each enemy's move is pruned individually). Medium risk: changes the paranoid
   model's semantics slightly (sequential rather than joint replies); their leaves-at-
   turn-boundaries rule keeps it sound.
8. **Time**: keep 350 ms with the fixed 150 ms padding model (we already target 350);
   their busy-poll + worker-halt pattern avoids one wasted budget share.

## Part 2 — External articles and papers (verified, with URLs)

- **coreyja, "Minimax in Battlesnake"** (2022) — the hobbs author's write-up:
  https://coreyja.com/posts/BattlesnakeMinimax/Minimax%20in%20Battlesnake
- **"The World's best Snake AI" — Shapeshifter post-mortem** (notpeerreviewed.com, 2024):
  https://notpeerreviewed.com/blog/battlesnake
  Alpha-beta modeled on chess engines; **Best Node Search** (BNS, Ibarra/Ruiz? — Rutko
  2011) over null-window calls "wins by a landslide" over MTD(f) in self-play — BNS
  binary-searches the value range until one root move survives, ideal when only the
  best *move* matters, not the value. Bitboards with u128 for 11×11 and heap-free
  stack bitsets for 25×25; "bit-shift-based floodfill is one of the most important and
  expensive heuristics". **Minimum Combination Search (MCS)**: covers every individual
  opponent move with the fewest joint combinations (branching 3 vs our 4^(n-1));
  60% win rate in 4- and 8-player games, but loses to BRS+ in self-play (blind spots).
- **Schadd, Winands, Herik, "Best-Reply Search for Multi-Player Games"** (2008):
  https://dke.maastrichtuniversity.nl/m.winands/documents/BestReplySearch.pdf
  BRS: only the strongest opponent replies at each node (not the joint product);
  beats maxn everywhere tested and paranoid in Chinese Checkers/Focus. Relevant to our
  4-snake FFA where 4^3=64 joint replies per node kill depth.
- **Zuckerman et al., "Mixing Search Strategies (MP-MIX)"** (IJCAI 2009 / 2011):
  https://ijcai.org/proceedings/2009/0113.pdf
  Dynamically switches paranoid ↔ maxn depending on whether opponents can actually
  hurt each other (leader-focused threat model). Better than pure paranoid in 3 games.
- **Schier, "Adversarial N-player Search using Locality"** (2019):
  Uses the same sorted-by-length flood-fill initialization as hobbs; note for the
  theory behind part 1's #1.
- **"Opponent-Pruning Paranoid Search (OPPS)"** (ACM 2021):
  https://dl.acm.org/doi/10.1145/3402942.3402957
  Prunes individual opponents' moves inside paranoid search.
- **chessprogramming.org** (canonical references we already follow): Transposition
  Table, Null Move Pruning, Lazy SMP:
  https://chessprogramming.org/Transposition_Table ·
  https://chessprogramming.org/Null_Move_Pruning ·
  https://chessprogramming.org/Lazy_SMP
  Plus "Transposition Tables and Improvements" (Chess Engine Lab):
  https://chessenginelab.substack.com/p/transposition-tables-and-improvements
- **Official docs**: https://docs.battlesnake.com/guides/useful-algorithms ·
  curated list: https://github.com/xtagon/awesome-battlesnake
- **Verified null-move pruning** (Björnsson & Marsland, arXiv 0808.1125): re-search at
  reduced depth when a null-move cutoff fires — same "trust but verify" shape as our
  LMR; a null-move analog for Battlesnake would be "skip my move" territory probes.

## Part 3 — Recommended plan (priority order, expected cost → payoff)

| # | Change | Where | Effort | Expected |
|---|---|---|---|---|
| 1 | **DONE (2026-11-26)** — tail-aware flood fill: vacate-schedule release (ring walk, head-ward-most occurrence for stacks), longest-first same-cycle claims, frontier-death, multi fill cap 12 + unbounded single fill; eval rewired, dFood kept static | `fastboard.cpp` `tailAwareVoronoi/tailAwareFlood`, `evalFast`, `tests/test_tailaware.cpp` | done | Self-play 85–15 vs v5; **snek-two 98% (49–1, was 94%)**; **hobbs flipped 0–4 → 3–1 / 2–2** @100ms — the predicted endgame gap closed |
| 2 | Weighted territory ratio (20/5/1) replacing flat Voronoi cells | `evalFast` | small | Better food/hazard space pricing (duels + royale) |
| 3 | Health cliff (lexicographic LowOnHealth below 60/85) | `evalFast` scoring categories | small | Removes starvation-race losses (our 3-per-60 losses are exactly this) |
| 4 | Length-vs-longest-living-rival ±3 term | `evalFast` | small | Safer growth decisions |
| 5 | Terminal prefs: mutual destruction + die-later lexicographic | `terminalValueFast` | small | Better trades in lost/won endgames |
| 6 | Tree-replay ordering seed (keep last PV + refutations, head-diff replay) | `Searcher` + `searchV2` entry | medium | Their ordering answer; complements our TT |
| 7 | BNS or BRS-style reply reduction for FFA (4+ snakes) | `searchTurn` reply enumeration | medium | FFA depth 4.2 → 6+; duel-neutral |
| 8 | One-ply-per-snake restructuring | search core | large | Finer cutoffs; only after 1-6 |
| 9 | Lazy SMP | search core | large, needs multi-core host | Skipped until a multi-core host exists |

Not recommended (verified absent or negative): copying their time padding (we already
budget 350 ms), TT removal (ours is proven value-preserving; theirs has none), opening
books/symmetry (they have none either), food-spawn modeling (both exclude it).
