#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <sstream>
#include <utility>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string.hpp>
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
    std::cout << "Waiting for the game to start...\n";

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

    std::cout << '\n';
    draw_game(umsg);

    last_umsg_ = umsg;
  }

  void on_server_disconnection() override {
    last_umsg_.playing = false;

    std::cout << "Client session finished.\n";
  }

 protected:
  void log(const std::string& msg) const {}

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

    // Game title
    // std::cout << enclose_text("TIC TAC TOE") << '\n';

    // Game board
    const std::string board = draw_board_str(umsg);

    // How to play
    std::string instructions = "";
    if (umsg.playing) {
      instructions +=
          "HOW TO PLAY\n"
          "Type a digit from your numeric pad (numpad) to choose a cell.\n"
          "Digits correspond to cells so that the game board resembles your "
          "numpad.\n";
    }

    // Game status information
    std::string status = "";
    if (umsg.playing) {
      status +=
          "Waiting for " + player_name[(int)umsg.current_player] + " to move";
    } else if (umsg.winner == ttt_player_id::none) {
      status += "GAME OVER, you tied!";
    } else {
      status += "GAME OVER, you ";
      status += (umsg.winner == umsg.player_id ? "won!" : "lost!");
    }
    status += "\n";

    // Draw it!
    std::cout << board << '\n';
    std::cout << instructions << '\n';
    std::cout << status << '\n';
  }

  /*
  ** 'Encloses' a given text around a box,
  ** whose edges are drawn with a given char
  */
  std::string enclose_text(const std::string str, char fill_char = '*') {
    std::deque<std::string> content;
    boost::split(content, str, boost::is_any_of("\n"));

    unsigned max_len = 0;
    for (auto line : content) {
      max_len = std::max((unsigned)line.size(), max_len);
    }

    std::string separator(max_len + 4, fill_char);

    if (content.front() != "") {
      content.push_front("");
    }
    if (content.back() != "") {
      content.push_back("");
    }

    std::stringstream result;

    result << separator << '\n';
    for (std::string line : content) {
      line = line + std::string(max_len - line.size(), ' ');
      result << fill_char << ' ' << line << ' ' << fill_char << '\n';
    }
    result << separator << '\n';

    return result.str();
  }

  /*
  ** Returns a text representation of the board inside a TTT Update Message
  */
  std::string draw_board_str(const ttt_update_message& umsg) {
    std::string board = empty_board;

    for (unsigned i = 0; i < ttt_board_side; i++) {
      for (unsigned j = 0; j < ttt_board_side; j++) {
        // Skip drawing if no player owns the spot
        if (umsg.board[i][j] == ttt_player_id::none) {
          continue;
        }

        // Get player's corresponding letter
        std::string letter = (umsg.board[i][j] == umsg.player_id ? X : O);

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
      "       \n \\   / \n  \\ /  \n   x   \n  / \\  \n /   \\ \n       \n";
  const std::string O =
      "   _   \n  / \\  \n |   | \n |   | \n |   | \n  \\_/  \n       \n";
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