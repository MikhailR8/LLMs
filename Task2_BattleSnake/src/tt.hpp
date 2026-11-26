// Transposition table for the v3 search.
//
// Two entry kinds share one direct-indexed table:
//   - search entries (Exact/Lower/Upper + remaining depth + best move):
//     the usual alpha-beta cache. Cross-turn reuse works because one real
//     game turn shifts every stored position one ply down the tree while
//     its absolute game turn (part of the key) rises by one - so along the
//     actually-played line the depths line up exactly.
//   - eval entries (flag Eval, depth 0): a "pawn-hash"-style cache of the
//     static leaf evaluation. The eval dominates node cost (collectChildren
//     evaluates every child), and iterative deepening re-expands interior
//     nodes every iteration, so the same positions are re-evaluated several
//     times per turn and again next turn.
//
// Replacement: same key always updates; an eval entry never clobbers a
// search entry; a shallower search entry never clobbles a deeper stranger
// (keeps last turn's deep entries alive). No aging: at ~3k stores per 25 ms
// search on a 2^16 table, entries survive ~20 turns, which covers the
// reuse window. 64-bit keys, engine-grade collision odds; entries from
// timed-out (incomplete) searches are simply never stored.
#pragma once

#include <cstdint>
#include <cstring>

namespace bs {

enum class TTFlag : uint8_t { Empty = 0, Exact, Lower, Upper, Eval };

struct TTEntry {
  uint64_t key;
  double value;
  uint8_t depth;  // remaining search depth (Eval entries: 0)
  uint8_t flag;   // TTFlag
  uint8_t move;   // Dir of the best move at the max node (search entries)
  uint8_t pad;
};

class TranspositionTable {
 public:
  static constexpr size_t kEntries = 1u << 16;
  static constexpr size_t kMask = kEntries - 1;

  // Returns the entry when the key matches and the slot is live.
  const TTEntry* probe(uint64_t key) const {
    const TTEntry& e = entries_[key & kMask];
    if (e.flag == static_cast<uint8_t>(TTFlag::Empty) || e.key != key) return nullptr;
    return &e;
  }

  void store(uint64_t key, double value, uint8_t depth, TTFlag flag, uint8_t move) {
    TTEntry& e = entries_[key & kMask];
    const bool eLive = e.flag != static_cast<uint8_t>(TTFlag::Empty);
    const bool eSearch = eLive && e.flag != static_cast<uint8_t>(TTFlag::Eval);
    const bool nSearch = flag != TTFlag::Eval;
    if (eLive) {
      if (e.key == key) {
        // same position: keep a deeper search result over a shallower one
        if (nSearch && eSearch && e.depth > depth) return;
        if (!nSearch && eSearch) return;  // keep the search entry
      } else {
        // strangers: eval entries never clobber, shallower never clobbers
        // a deeper search entry (keeps last turn's deep entries alive)
        if (!nSearch) return;
        if (eSearch && e.depth > depth) return;
      }
    }
    e.key = key;
    e.value = value;
    e.depth = depth;
    e.flag = static_cast<uint8_t>(flag);
    e.move = move;
  }

  size_t entries() const { return kEntries; }

  void clear() { std::memset(entries_, 0, sizeof(entries_)); }

 private:
  TTEntry entries_[kEntries] = {};
};

// Process-wide table (cross-game reuse is sound: the key pins board shape,
// viewer, hazard damage, turn, food and every body sequence; the value of a
// position does not depend on which game it appeared in).
TranspositionTable& globalTT();

}  // namespace bs
