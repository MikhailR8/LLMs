#include "tt.hpp"

namespace bs {

TranspositionTable& globalTT() {
  static TranspositionTable table;  // ~1.5 MB, zero-initialized
  return table;
}

}  // namespace bs
