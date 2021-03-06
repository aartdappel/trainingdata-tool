﻿#include "chess/position.h"
#include "move.h"
#include "move_do.h"
#include "move_gen.h"
#include "move_legal.h"
#include "neural/encoder.h"
#include "neural/network.h"
#include "neural/writer.h"
#include "pgn.h"
#include "polyglot_lib.h"
#include "san.h"
#include "square.h"
#include "util.h"

#include <cstring>
#include <iostream>

uint64_t resever_bits_in_bytes(uint64_t v) {
  v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
  v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
  v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
  return v;
}

lczero::Move poly_move_to_lc0_move(move_t move, board_t* board) {
  lczero::BoardSquare from(square_rank(move_from(move)),
                           square_file(move_from(move)));
  lczero::BoardSquare to(square_rank(move_to(move)),
                         square_file(move_to(move)));
  lczero::Move m(from, to);

  if (move_is_promote(move)) {
    lczero::Move::Promotion lookup[5] = {
        lczero::Move::Promotion::None,   lczero::Move::Promotion::Knight,
        lczero::Move::Promotion::Bishop, lczero::Move::Promotion::Rook,
        lczero::Move::Promotion::Queen,
    };
    auto prom = lookup[move >> 12];
    m.SetPromotion(prom);
  } else if (move_is_castle(move, board)) {
    bool is_short_castle =
        square_file(move_from(move)) < square_file(move_to(move));
    int file_to = is_short_castle ? 6 : 2;
    m.SetTo(lczero::BoardSquare(square_rank(move_to(move)), file_to));
    m.SetCastling();
  }

  if (colour_is_black(board->turn)) {
    m.Mirror();
  }

  return m;
}

lczero::V4TrainingData get_v4_training_data(
    lczero::GameResult game_result, const lczero::PositionHistory& history,
    lczero::Move played_move, lczero::MoveList legal_moves) {
  lczero::V4TrainingData result;

  // Set version.
  result.version = 4;

  // Illegal moves will have "-1" probability
  std::memset(result.probabilities, -1.0f, sizeof(result.probabilities));

  // Populate legal moves with probability "0"
  for (lczero::Move move : legal_moves) {
    result.probabilities[move.as_nn_index()] = 0;
  }

  // Assign "1" (100%) to the move that was actually played
  result.probabilities[played_move.as_nn_index()] = 1.0f;

  // Populate planes.
  lczero::InputPlanes planes = EncodePositionForNN(history, 8, lczero::FillEmptyHistory::FEN_ONLY);
  int plane_idx = 0;
  for (auto& plane : result.planes) {
    plane = resever_bits_in_bytes(planes[plane_idx++].mask);
  }

  const auto& position = history.Last();
  // Populate castlings.
  result.castling_us_ooo =
      position.CanCastle(lczero::Position::WE_CAN_OOO) ? 1 : 0;
  result.castling_us_oo =
      position.CanCastle(lczero::Position::WE_CAN_OO) ? 1 : 0;
  result.castling_them_ooo =
      position.CanCastle(lczero::Position::THEY_CAN_OOO) ? 1 : 0;
  result.castling_them_oo =
      position.CanCastle(lczero::Position::THEY_CAN_OO) ? 1 : 0;

  // Other params.
  result.side_to_move = position.IsBlackToMove() ? 1 : 0;
  result.move_count = 0;
  result.rule50_count = position.GetNoCaptureNoPawnPly();

  // Game result.
  if (game_result == lczero::GameResult::WHITE_WON) {
    result.result = position.IsBlackToMove() ? -1 : 1;
  } else if (game_result == lczero::GameResult::BLACK_WON) {
    result.result = position.IsBlackToMove() ? 1 : -1;
  } else {
    result.result = 0;
  }

  return result;
}

void write_one_game_training_data(pgn_t* pgn, int game_id) {
  std::vector<lczero::V4TrainingData> training_data;
  lczero::ChessBoard starting_board;
  const std::string starting_fen =
      std::strlen(pgn->fen) > 0 ? pgn->fen : lczero::ChessBoard::kStartposFen;
  starting_board.SetFromFen(starting_fen, nullptr, nullptr);
  lczero::PositionHistory position_history;
  position_history.Reset(starting_board, 0, 0);
  board_t board[1];
  board_start(board);
  char str[256];
  lczero::TrainingDataWriter writer(game_id);

  lczero::GameResult game_result;
  if (my_string_equal(pgn->result, "1-0")) {
    game_result = lczero::GameResult::WHITE_WON;
  } else if (my_string_equal(pgn->result, "0-1")) {
    game_result = lczero::GameResult::BLACK_WON;
  } else {
    game_result = lczero::GameResult::DRAW;
  }

  while (pgn_next_move(pgn, str, 256)) {
    // Extract move from pgn
    int move = move_from_san(str, board);
    if (move == MoveNone || !move_is_legal(move, board)) {
      std::cout << "illegal move \"" << str << "\" at line " << pgn->move_line
                << ", column " << pgn->move_column;
      break;
    }

    // Convert move to lc0 format
    lczero::Move lc0_move = poly_move_to_lc0_move(move, board);
    
    bool found = false;
    auto legal_moves = position_history.Last().GetBoard().GenerateLegalMoves();
    for (auto legal : legal_moves) {
      if (legal == lc0_move && legal.castling() == lc0_move.castling()) {
        found = true;
        break;
      }
    }
    if (!found) {
      std::cout << "Move not found: " << str << " " << game_id << " "
                << square_file(move_to(move)) << std::endl;
    }

    // Generate training data
    lczero::V4TrainingData chunk =
        get_v4_training_data(game_result, position_history, lc0_move, legal_moves);

    // Execute move
    position_history.Append(lc0_move);
    move_do(board, move);

    writer.WriteChunk(chunk);
  }

  writer.Finalize();
}

int main(int argc, char* argv[]) {
  lczero::InitializeMagicBitboards();
  polyglot_init();
  int game_id = 0;
  while (*++argv) {
    printf("%s\n", *argv);
    pgn_t pgn[1];
    pgn_open(pgn, *argv);
    while (pgn_next_game(pgn)) {
      write_one_game_training_data(pgn, game_id++);
    }
    pgn_close(pgn);
  }
}