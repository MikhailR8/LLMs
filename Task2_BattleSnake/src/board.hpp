// Bitboard primitives for the v3 fast search path.
//
// Cells are linearized as idx = y * W + x (y grows upward, matching the
// engine's Point convention). A Bits is a fixed-capacity multiword bitset;
// the active word count comes from BitGeom, so small boards (11x11 -> 3
// words incl. shift headroom) stay cheap.
//
// Board shifts (logical shifts of the whole multiword integer):
//   up    (y+1) : bits << W, masked to valid cells
//   down  (y-1) : bits >> W
//   left  (x-1) : clear column 0 first, then >> 1 (kills row wraparound)
//   right (x+1) : clear column W-1 first, then << 1
#pragma once

#include <cstdint>
#include <cstring>

namespace bs {

constexpr int kMaxBoardCells = 512;  // 21x21 royale + slack
constexpr int kMaxBitWords = 8;      // 512 / 64
constexpr int kMaxFastSnakes = 8;
constexpr int kMaxRing = 1024;       // body ring capacity (power of two)
constexpr int kRingMask = kMaxRing - 1;
constexpr uint16_t kOOBCell = 0xFFFF;  // sentinel: off-board head

struct Bits {
  uint64_t w[kMaxBitWords] = {};

  void clearAll() { std::memset(w, 0, sizeof(w)); }
  void set(int idx) { w[idx >> 6] |= 1ull << (idx & 63); }
  void reset(int idx) { w[idx >> 6] &= ~(1ull << (idx & 63)); }
  bool test(int idx) const { return (w[idx >> 6] >> (idx & 63)) & 1ull; }
  bool none() const {
    for (int i = 0; i < kMaxBitWords; ++i)
      if (w[i]) return false;
    return true;
  }
  int popcount() const {
    int c = 0;
    for (int i = 0; i < kMaxBitWords; ++i) c += __builtin_popcountll(w[i]);
    return c;
  }
  Bits operator|(const Bits& o) const {
    Bits r;
    for (int i = 0; i < kMaxBitWords; ++i) r.w[i] = w[i] | o.w[i];
    return r;
  }
  Bits operator&(const Bits& o) const {
    Bits r;
    for (int i = 0; i < kMaxBitWords; ++i) r.w[i] = w[i] & o.w[i];
    return r;
  }
  Bits operator~() const {
    Bits r;
    for (int i = 0; i < kMaxBitWords; ++i) r.w[i] = ~w[i];
    return r;
  }
  Bits& operator|=(const Bits& o) {
    for (int i = 0; i < kMaxBitWords; ++i) w[i] |= o.w[i];
    return *this;
  }
  Bits& operator&=(const Bits& o) {
    for (int i = 0; i < kMaxBitWords; ++i) w[i] &= o.w[i];
    return *this;
  }
  bool operator==(const Bits& o) const {
    return std::memcmp(w, o.w, sizeof(w)) == 0;
  }
};

struct BitGeom {
  int W = 0, H = 0, cells = 0, words = 0;
  Bits colFirst, colLast, valid;

  static BitGeom make(int W, int H) {
    BitGeom g;
    g.W = W;
    g.H = H;
    g.cells = W * H;
    g.words = (g.cells + 63) / 64 + 1;  // +1 spare word for left shifts
    g.colFirst.clearAll();
    g.colLast.clearAll();
    g.valid.clearAll();
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        int idx = y * W + x;
        g.valid.set(idx);
        if (x == 0) g.colFirst.set(idx);
        if (x == W - 1) g.colLast.set(idx);
      }
    return g;
  }
  int idx(int x, int y) const { return y * W + x; }
  int xOf(int idx) const { return idx % W; }
  int yOf(int idx) const { return idx / W; }
};

// Shifts every bit by +delta cell indices (delta >= 0) with cross-word
// carries; garbage above `words` is masked away by callers via BitGeom::valid.
inline Bits shiftIdxUp(const Bits& b, const BitGeom& g, int delta) {
  Bits r;
  r.clearAll();
  const int wordShift = delta >> 6, bitShift = delta & 63;
  for (int i = g.words - 1; i >= 0; --i) {
    const int src = i - wordShift;
    uint64_t v = 0;
    if (src >= 0) {
      v = b.w[src] << bitShift;
      if (bitShift && src >= 1) v |= b.w[src - 1] >> (64 - bitShift);
    }
    r.w[i] = v;
  }
  return r;
}

inline Bits shiftIdxDown(const Bits& b, const BitGeom& g, int delta) {
  Bits r;
  r.clearAll();
  const int wordShift = delta >> 6, bitShift = delta & 63;
  for (int i = 0; i < g.words; ++i) {
    const int src = i + wordShift;
    uint64_t v = 0;
    if (src < g.words) {
      v = b.w[src] >> bitShift;
      if (bitShift && src + 1 < g.words) v |= b.w[src + 1] << (64 - bitShift);
    }
    r.w[i] = v;
  }
  return r;
}

inline Bits moveUp(const Bits& b, const BitGeom& g) {
  return shiftIdxUp(b, g, g.W) & g.valid;
}
inline Bits moveDown(const Bits& b, const BitGeom& g) {
  return shiftIdxDown(b, g, g.W);
}
inline Bits moveLeft(const Bits& b, const BitGeom& g) {
  return shiftIdxDown(b & ~g.colFirst, g, 1);
}
inline Bits moveRight(const Bits& b, const BitGeom& g) {
  return shiftIdxUp(b & ~g.colLast, g, 1) & g.valid;
}
inline Bits neighbors(const Bits& b, const BitGeom& g) {
  return moveUp(b, g) | moveDown(b, g) | moveLeft(b, g) | moveRight(b, g);
}

}  // namespace bs
