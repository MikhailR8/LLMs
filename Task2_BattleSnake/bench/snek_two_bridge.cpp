// Bridge driver for the cloned snek-two opponent (2018 Advanced Division
// winner, https://github.com/aleksiy325/snek-two).
//
// Reads one 2018-era Battlesnake move request (JSON) on stdin, runs the
// opponent's MinimaxSnake::decideMove unchanged, and prints its response
// JSON on stdout. This avoids running their Crow server: we link their
// strategy code directly and speak their exact request schema.
//
// stdout may contain debug noise from their parser (it prints the snake id);
// the response line is the one starting with '{'.
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

#include "../../opponents/snek-two/src/utils/server_utils.cpp"          // parseGameState, encodeMove
#include "../../opponents/snek-two/src/strategies/minimax_snake.cpp"    // MinimaxSnake::decideMove

int main() {
  std::ios::sync_with_stdio(false);
  std::string body((std::istreambuf_iterator<char>(std::cin)),
                   std::istreambuf_iterator<char>());
  try {
    std::pair<GameState, snake_index> info = parseGameState(body);
    GameState gs = info.first;
    assert(gs.isGivenValid());
    snake_index idx = info.second;
    MinimaxSnake minimax_snake;
    Direction move = minimax_snake.decideMove(gs, idx);
    std::cout << encodeMove(move) << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bridge error: %s\n", e.what());
    return 1;
  }
}
