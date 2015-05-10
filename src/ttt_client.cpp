#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <sstream>
#include <utility>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include "ttt_shared.hpp"

using boost::asio::ip::tcp;

//------------------------------------------------------------------------------

class ttt_client_base {
 public:
  ttt_client_base(boost::asio::io_service& io_service,
                  tcp::resolver::iterator endpoint_iterator)
      : io_service_(io_service), socket_(io_service) {
    do_connect(endpoint_iterator);
  }

  void close() {
    static bool got_here = false;
    if (got_here) {
      return;
    }
    got_here = true;

    socket_.shutdown(tcp::socket::shutdown_both);
    socket_.close();

    log("Disconnected from the server");
    on_server_disconnection();
  }

 protected:
  virtual void on_server_connection() {}

  virtual void on_message_received(const ttt_message& msg) {}

  virtual void on_message_sent(const ttt_message& msg) {}

  virtual void on_server_disconnection() {}

  virtual void log(const std::string& msg) const {
    std::string buffer = "tic_tac_toe_client '" + msg + "'\n";
    std::cout << buffer;
  }

  void write(const ttt_message& msg) {
    io_service_.post([this, msg]() {
      bool write_in_progress = !write_msgs_.empty();
      write_msgs_.push_back(msg);
      if (!write_in_progress) {
        do_write();
      }
    });
  }

 private:
  void do_connect(tcp::resolver::iterator endpoint_iterator) {
    boost::asio::async_connect(
        socket_, endpoint_iterator,
        [this](boost::system::error_code ec, tcp::resolver::iterator) {
          if (!ec) {
            log("Connected to the server");
            on_server_connection();

            do_read_header();
          } else {
            log("Could not connect to the server");
          }
        });
  }

  void do_read_header() {
    boost::asio::async_read(
        socket_,
        boost::asio::buffer(read_msg_.data(), ttt_message::header_length),
        [this](boost::system::error_code ec, std::size_t /*length*/) {
          if (!ec && read_msg_.decode_header()) {
            do_read_body();
          } else {
            log("An error occurred while listening to the server");
            this->close();
          }
        });
  }

  void do_read_body() {
    boost::asio::async_read(
        socket_, boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
        [this](boost::system::error_code ec, std::size_t /*length*/) {
          if (!ec) {
            log("Received server message");
            on_message_received(read_msg_);

            do_read_header();
          } else {
            log("An error occurred while listening to the server");
            this->close();
          }
        });
  }

  void do_write() {
    log("Sending message...");
    boost::asio::async_write(
        socket_, boost::asio::buffer(write_msgs_.front().data(),
                                     write_msgs_.front().length()),
        [this](boost::system::error_code ec, std::size_t /*length*/) {
          if (!ec) {
            log("A message was sent");
            on_message_sent(write_msgs_.front());

            write_msgs_.pop_front();

            if (!write_msgs_.empty()) {
              do_write();
            }
          } else {
            log("An error occurred while writing to the server");
            this->close();
          }
        });
  }

 private:
  boost::asio::io_service& io_service_;
  tcp::socket socket_;
  ttt_message read_msg_;
  ttt_message_queue write_msgs_;
};

//------------------------------------------------------------------------------

class ttt_client : public ttt_client_base {
 public:
  ttt_client(boost::asio::io_service& io_service,
             tcp::resolver::iterator endpoint_iterator)
      : ttt_client_base(io_service, endpoint_iterator) {}

  void take(int x, int y) {
    std::stringstream ss;
    ss << x << ", " << y;

    std::string msg_body = ss.str();

    ttt_message msg;
    memcpy(msg.body(), msg_body.c_str(), msg_body.size());
    msg.body_length(msg_body.size());
    msg.encode_header();

    write(msg);
  }

 protected:
  void on_server_connection() override {
    std::cout << "WAITING FOR THE GAME TO START...\n";

    last_umsg_.playing = true;

    boost::thread input_t([this]() {
      while (this->last_umsg_.playing) {  // While the game is running...
        std::string token;
        std::cin >> token;
        
        int value;
        bool got_int = (std::sscanf(token.c_str(), "%d", &value) == 1);
        
        if (got_int && 1 <= value && value <= 9) {
          int x = this->numpad_to_cell[value].first;
          int y = this->numpad_to_cell[value].second;
          this->take(x, y);
        }
      }
    });

    input_t.detach();
  }

  void on_message_received(const ttt_message& msg) override {
    ttt_update_message umsg;

    if (!ttt_update_message::try_parse(msg, umsg)) {
      return;
    }

    draw_game(umsg);

    last_umsg_ = umsg;
  }

  void on_server_disconnection() override { last_umsg_.playing = false; }

 private:
  /*
  ** Draws a representation of a TTT game from an TTT Update Message
  */
  void draw_game(const ttt_update_message& umsg) {
    // Players naming
    std::string player_name[ttt_number_of_players];
    for (int i = 0; i < ttt_number_of_players; i++) {
      player_name[i] = (i == (int)umsg.player_id ? "you" : "your opponent");
    }

    // Game header
    const std::string header = "Player 1 (Xs, " + player_name[0] +
                               ") vs Player 2 (Os, " + player_name[1] + ")";

    const std::string header_line(header.size(), '*');

    std::cout << '\n';
    std::cout << header_line << '\n' << header << '\n' << header_line << "\n\n";

    // Game board
    const std::string board = board_from(umsg);

    std::cout << board << '\n';

    // Instructions
    if (umsg.current_player == umsg.player_id) {
      std::cout
          << "Type a digit from your numeric pad (numpad) to choose a cell.\n";
      std::cout << "Digits correspond to cells so that the game board "
                   "resembles the numpad.\n\n";
    }
    // Game's next step
    if (umsg.playing) {
      std::cout << "Waiting for " << player_name[(int)umsg.current_player]
                << " to move";
    } else if (umsg.winner == ttt_player_id::none) {
      std::cout << "GAME OVER, you tied!";
    } else {
      std::cout << "GAME OVER, you "
                << (umsg.winner == umsg.player_id ? "won!" : "lost!");
    }
    std::cout << "\n\n";
  }

  /*
  ** Returns a text representation of the board inside a TTT Update Message
  */
  std::string board_from(const ttt_update_message& umsg) {
    std::string board = empty_board;

    for (unsigned i = 0; i < ttt_board_side; i++) {
      for (unsigned j = 0; j < ttt_board_side; j++) {
        // Skip drawing if no player owns the spot
        if (umsg.board[i][j] == ttt_player_id::none) {
          continue;
        }

        // Get player's corresponding letter
        std::string letter =
            (umsg.board[i][j] == ttt_player_id::player_1 ? X : O);

        // Draw it!
        const int x = 8 * i, y = 8 * j;
        for (unsigned a = 0, b = 0; a < letter.size(); a++) {
          if (letter[a] == '\n') {
            b += 1;
            continue;
          }

          const int row = x + b, col = y + (a % 8);
          board[row * 24 + col] = letter[a];
        }
      }
    }

    return board;
  }

 private:
  ttt_update_message last_umsg_;
  const std::string X =
      "       \n o   o \n  o o  \n   o   \n  o o  \n o   o \n       \n";
  const std::string O =
      "       \n  ooo  \n o   o \n o   o \n o   o \n  ooo  \n       \n";
  const std::string empty_board =
      "       |       |       \n       |       |       \n       |       |    "
      "   \n       |       |       \n       |       |       \n       |       "
      "|       \n       |       |       \n-----------------------\n       |  "
      "     |       \n       |       |       \n       |       |       \n     "
      "  |       |       \n       |       |       \n       |       |       "
      "\n       |       |       \n-----------------------\n       |       |  "
      "     \n       |       |       \n       |       |       \n       |     "
      "  |       \n       |       |       \n       |       |       \n       "
      "|       |       \n";
  const std::pair<int, int> numpad_to_cell[10] = {{9, 9},
                                                  {2, 0},
                                                  {2, 1},
                                                  {2, 2},
                                                  {1, 0},
                                                  {1, 1},
                                                  {1, 2},
                                                  {0, 0},
                                                  {0, 1},
                                                  {0, 2}};
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  try {
    if (argc != 3) {
      std::cerr << "Usage: client <host> <port>\n";
      return 1;
    }

    boost::asio::io_service io_service;

    tcp::resolver resolver(io_service);
    auto endpoint_iterator = resolver.resolve({argv[1], argv[2]});
    ttt_client c(io_service, endpoint_iterator);

    boost::thread client_t([&io_service]() { io_service.run(); });

    client_t.join();
  } catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}