#include <cstdlib>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <map>
#include <utility>
#include <string>
#include <exception>
#include <vector>
#include <functional>
#include <sstream>
#include <boost/asio.hpp>
#include "ttt_shared.hpp"

using boost::asio::ip::tcp;
using server_log_func = std::function<void(const std::string&)>;
using server_do_accept_func = std::function<void()>;

//------------------------------------------------------------------------------

class ttt_player {
 public:
  virtual ~ttt_player(){};
  virtual void start() = 0;
  virtual void close() = 0;
  virtual void deliver(const ttt_message& msg) = 0;
};

//------------------------------------------------------------------------------

class ttt_game {
 public:
  ttt_game(server_log_func log, server_do_accept_func do_accept)
      : playing_(false), log_(log), do_accept_(do_accept) {}

  /*
  ** Is there a game running?
  */
  bool playing() const { return playing_; }

  /*
  ** Is this game looking for players?
  */
  bool looking_for_players() const {
    return !playing() && players_.size() != ttt_number_of_players;
  }

  /*
  ** Starts a new game
  */
  void start_game() {
    if (looking_for_players()) {
      throw new std::logic_error("Game needs player(s)");
    }

    log_("Game started!");

    clear_board();
    current_player(ttt_player_id::player_1);
    winner_ = ttt_player_id::none;
    playing_ = true;

    deliver_game_state();
  }

  /*
  ** Starts a new game only if conditions are ideal
  */
  void try_start_game() {
    if (!playing() && !looking_for_players()) {
      start_game();
    }
  }

  /*
  ** Adds the given player to the game. Then tries to start it
  */
  void add_player(std::shared_ptr<ttt_player> player) {
    if (!looking_for_players()) {
      throw new std::logic_error("Game is not looking for players");
    }

    if (in_game(player)) {
      return;  // Ignore the request if the player is already in-game
    }

    if (players_.size() == 0) {
      players_.insert(make_pair(player, ttt_player_id::player_1));  // Add P1
    } else {
      auto first_player = players_.begin()->first;
      players_[first_player] = ttt_player_id::player_1;  // Ensure we have P1

      players_.insert(make_pair(player, ttt_player_id::player_2));  // Add P2
    }

    player->start();

    try_start_game();
  }

  /*
  **  Removes the given player from the game. Then tries to end it
  */
  void remove_player(std::shared_ptr<ttt_player> player) {
    if (!in_game(player)) {
      return;  // Ignore the request if the player is not in-game
    }

    if (playing()) {
      ttt_player_id pid = player_id(player);

      std::string text = "Player " + std::to_string((int)pid + 1) + " quitted";
      log_(text);

      end_game();
    } else {
      log_("A player left the game");

      player->close();
      players_.erase(player);
    }
  }

  /*
  ** Try to make a move with a specific player
  */
  void try_move(std::shared_ptr<ttt_player> player, int x, int y) {
    if (!playing()) {
      return; // Ignore the request if there's no game running
    }
    
    const ttt_player_id pid = player_id(player);

    if (pid == ttt_player_id::none) {
      return;  // Ignore the request if this is an invalid player
    }

    if (pid != current_player_) {
      return;  // Ignore the request if this is not the current player
    }

    if (x < 0 || x >= ttt_board_side || y < 0 || y >= ttt_board_side) {
      return;  // Ignore the request if the cell is invalid
    }

    if (board_[x][y] != ttt_player_id::none) {
      return;  // Ignore the request if the given cell is already owned
    }

    // Log the move
    std::stringstream ss;
    ss << "Player " << (int(pid) + 1) << " gets cell " << x << ", " << y;
    log_(ss.str());

    // Process the move
    board_[x][y] = pid;
    update_game_state();

    // Will the game continue?
    if (playing()) {
      current_player(next_player());  // Setup for the next turn
      deliver_game_state();
      return;
    }

    // Game over!
    deliver_game_state();
    
    std::string text = "Players tied!";
    if (winner_ != ttt_player_id::none) {
      text = "Player " + std::to_string((int)winner_ + 1) + " wins!";
    }
    log_(text);
    end_game();
  }

  /*
  ** Ends the current game and starts the setup for another
  */
  void end_game() {
    if (looking_for_players()) {
      throw new std::logic_error("There is no game to properly end");
    }

    log_("Game over");

    playing_ = false;

    for (auto player : players_) {
      player.first->close();
    }
    players_.clear();

    do_accept_();
  }

 private:
  /*
  ** Checks if ending conditions have been met,
  ** and updates the game state correspondingly
  */
  void update_game_state() {
    // Is there a winner?

    std::vector<ttt_player_id> results;

    for (unsigned i = 0; i < ttt_board_side; i++) {
      results.push_back(player_on(i, 0, 0, 1));
      results.push_back(player_on(0, i, 1, 0));
    }
    results.push_back(player_on(0, 0, 1, 1));
    results.push_back(player_on(0, ttt_board_side - 1, 1, -1));

    for (auto player : results) {
      if (player != ttt_player_id::none) {
        winner_ = player;
        playing_ = false;
        return;
      }
    }

    // Is there a tie?

    unsigned n_empty_cells = 0;
    for (unsigned i = 0; i < ttt_board_side; i++) {
      for (unsigned j = 0; j < ttt_board_side; j++) {
        if (board_[i][j] == ttt_player_id::none) {
          n_empty_cells += 1;
        }
      }
    }

    if (n_empty_cells == 0) {
      winner_ = ttt_player_id::none;
      playing_ = false;
    }
  }

  /*
  ** Iterates 'ttt_board_side' cells of the form (sx, sy),
  ** (sx + dx, sy + dy), (sx + 2 * dx, sy + 2 *dy), ...
  ** and returns the player that is lying over all of them.
  ** Returns 'none' if no player is doing so.
  */
  ttt_player_id player_on(int sx, int sy, int dx, int dy) {
    ttt_player_id kind = ttt_player_id::none;
    unsigned cnt = 0;

    for (unsigned i = 0; i < ttt_board_side; i++) {
      int x = sx + i * dx;
      if (x < 0 || x >= ttt_board_side) {
        continue;
      }

      int y = sy + i * dy;
      if (y < 0 || y >= ttt_board_side) {
        continue;
      }

      if (i == 0) {
        kind = board_[x][y];
      }

      if (board_[x][y] == kind) {
        cnt += 1;
      }
    }

    bool found_player = (kind != ttt_player_id::none && cnt == ttt_board_side);
    return (found_player ? kind : ttt_player_id::none);
  }

  /*
  ** Clears the game board
  */
  void clear_board() {
    for (unsigned i = 0; i < board_.size(); i++) {
      for (unsigned j = 0; j < board_[i].size(); j++) {
        board_[i][j] = ttt_player_id::none;
      }
    }
  }

  /*
  ** Send an update to all players of the current game status
  */
  void deliver_game_state() {
    if (looking_for_players()) {
      return;  // Skip delivery if there is no game to inform about
    }

    for (auto player_id_pair : players_) {
      ttt_player_id pid = player_id_pair.second;

      ttt_update_message umsg(playing_, pid, current_player_, winner_, board_);

      ttt_message msg = umsg.to_message();

      player_id_pair.first->deliver(msg);
    }
  }

  /*
  ** Is the given player in the game?
  */
  bool in_game(std::shared_ptr<ttt_player> player) const {
    return players_.count(player) > 0;
  }

  /*
  ** Get player's id
  */
  ttt_player_id player_id(std::shared_ptr<ttt_player> p) const {
    if (players_.count(p) > 0) {
      return players_.at(p);
    }
    return ttt_player_id::none;
  }

  /*
  ** Returns the succesor of the current player
  */
  ttt_player_id next_player() {
    if (current_player_ == ttt_player_id::player_1) {
      return ttt_player_id::player_2;
    }
    return ttt_player_id::player_1;
  }

  /*
  ** Sets the current player (the one whos turn is going on)
  */
  void current_player(ttt_player_id pid) {
    std::string text =
        "Waiting for Player " + std::to_string((int)pid + 1) + " to move";
    log_(text);

    current_player_ = pid;
  }

 private:
  bool playing_ = false;
  ttt_board board_;
  ttt_player_id current_player_;
  ttt_player_id winner_;

  std::map<std::shared_ptr<ttt_player>, ttt_player_id>
      players_;                      // players pool
  server_log_func log_;              // Server log function
  server_do_accept_func do_accept_;  // Server do_accept function
};

//------------------------------------------------------------------------------

class ttt_remote_player : public std::enable_shared_from_this<ttt_player>,
                          public ttt_player {
 public:
  ttt_remote_player(tcp::socket socket, ttt_game& game)
      : socket_(std::move(socket)), game_(game) {}

  void start() { do_read_header(); }

  void deliver(const ttt_message& msg) {
    bool write_in_progress = !write_msgs_.empty();
    write_msgs_.push_back(msg);
    if (!write_in_progress) {
      do_write();
    }
  }

  void close() {
    socket_.shutdown(tcp::socket::shutdown_both);
    socket_.close();
  }

 private:
  void do_read_header() {
    auto self(shared_from_this());
    boost::asio::async_read(
        socket_,
        boost::asio::buffer(read_msg_.data(), ttt_message::header_length),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
          if (!ec && read_msg_.decode_header()) {
            do_read_body();
          } else {
            game_.remove_player(shared_from_this());
          }
        });
  }

  void do_read_body() {
    auto self(shared_from_this());
    boost::asio::async_read(
        socket_, boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
          if (!ec) {
            int x, y;
            if (std::sscanf(read_msg_.body(), "%d, %d", &x, &y) == 2) {
              game_.try_move(shared_from_this(), x, y);
            }
            do_read_header();
          } else {
            game_.remove_player(shared_from_this());
          }
        });
  }

  void do_write() {
    auto self(shared_from_this());
    boost::asio::async_write(
        socket_, boost::asio::buffer(write_msgs_.front().data(),
                                     write_msgs_.front().length()),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
          if (!ec) {
            write_msgs_.pop_front();
            if (!write_msgs_.empty()) {
              do_write();
            }
          } else {
            game_.remove_player(shared_from_this());
          }
        });
  }

 private:
  tcp::socket socket_;
  ttt_game& game_;
  ttt_message read_msg_;
  ttt_message_queue write_msgs_;
};

//------------------------------------------------------------------------------

class ttt_server {
 public:
  ttt_server(boost::asio::io_service& io_service, const tcp::endpoint& endpoint)
      : acceptor_(io_service, endpoint),
        socket_(io_service),
        game_([this](const std::string& msg) { this->log(msg); },
              [this]() { this->do_accept(); }) {
    do_accept();
  }

 private:
  void do_accept() {
    log("Looking for a player...");
    acceptor_.async_accept(socket_, [this](boost::system::error_code ec) {
      if (!ec) {
        log("A player joined the game");

        auto player =
            std::make_shared<ttt_remote_player>(std::move(socket_), game_);
        game_.add_player(player->shared_from_this());
      }

      if (game_.looking_for_players()) {
        do_accept();
      }
    });
  }

  void log(const std::string& msg) const {
    std::string buffer = "tic_tac_toe_server::" +
                         std::to_string(acceptor_.local_endpoint().port()) +
                         " '" + msg + "'\n";
    std::cout << buffer;
  }

 private:
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  ttt_game game_;
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  try {
    if (argc < 2) {
      std::cerr << "Usage: server <port> [<port> ...]\n";
      return 1;
    }

    boost::asio::io_service io_service;

    std::list<ttt_server> servers;
    for (int i = 1; i < argc; ++i) {
      tcp::endpoint endpoint(tcp::v4(), std::atoi(argv[i]));
      servers.emplace_back(io_service, endpoint);
    }

    io_service.run();
  } catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}