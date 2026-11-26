// Debug driver for the snek-two bridge: replicates MinimaxSnake::decideMove
// at the root but prints the minimax value of every candidate move, so we
// can see why their engine picks what it picks on a given request.
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

#include "../../opponents/snek-two/src/utils/server_utils.cpp"
#include "../../opponents/snek-two/src/strategies/minimax_snake.cpp"

int main() {
  std::ios::sync_with_stdio(false);
  std::string body((std::istreambuf_iterator<char>(std::cin)),
                   std::istreambuf_iterator<char>());
  std::pair<GameState, snake_index> info = parseGameState(body);
  GameState gs = info.first;
  snake_index idx = info.second;
  MinimaxSnake s;
  s.start = clock();

  std::printf("turn=%d snakes=%zu scoreState(root)=%g\n", gs.getTick(),
              gs.getSnakes().size(), s.scoreState(gs, idx));
  for (auto d : DIRECTIONS) {
    GameState ns = gs;
    ns.makeMove(d, idx);
    const bool aliveNow = ns.getSnake(idx).isAlive();
    GameState ct = ns;
    ct.cleanup();
    const bool aliveAfterCleanup = ct.getSnake(idx).isAlive();
    std::pair<double, Direction> r =
        s.alphabeta(ns, idx, d, std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::max(), 1, 4, false);
    std::printf("  move %-5s aliveNow=%d aliveAfterCleanup=%d minimax=%g\n",
                DIR_STR_MAP[d].c_str(), aliveNow, aliveAfterCleanup, r.first);
  }
  std::printf("decideMove says: %s\n",
              DIR_STR_MAP[s.decideMove(gs, idx)].c_str());
  return 0;
}
