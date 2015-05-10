#ifndef ttt_shared_hpp
#define ttt_shared_hpp

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <exception>
#include <vector>
#include <array>
#include <sstream>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

//----------------------------------------------------------------------

class ttt_message;
enum { ttt_board_side = 3, ttt_number_of_players = 2 };
enum class ttt_player_id { player_1 = 0, player_2 = 1, none = 2 };
typedef std::array<std::array<ttt_player_id, 3>, 3> ttt_board;
typedef std::deque<ttt_message> ttt_message_queue;

//----------------------------------------------------------------------

class ttt_message {
 public:
  enum { header_length = 4 };
  enum { max_body_length = 768 };

  ttt_message() : body_length_(0) {}

  const char* data() const { return data_; }

  char* data() { return data_; }

  std::size_t length() const { return header_length + body_length_; }

  const char* body() const { return data_ + header_length; }

  char* body() { return data_ + header_length; }

  std::size_t body_length() const { return body_length_; }

  void body_length(std::size_t new_length) {
    body_length_ = new_length;
    if (body_length_ > max_body_length) {
      body_length_ = max_body_length;
    }
  }

  bool decode_header() {
    char header[header_length + 1] = "";
    std::strncat(header, data_, header_length);
    body_length_ = std::atoi(header);
    if (body_length_ > max_body_length) {
      body_length_ = 0;
      return false;
    }
    return true;
  }

  void encode_header() {
    char header[header_length + 1] = "";
    std::sprintf(header, "%4d", static_cast<int>(body_length_));
    std::memcpy(data_, header, header_length);
  }

 private:
  char data_[header_length + max_body_length];
  std::size_t body_length_;
};

//----------------------------------------------------------------------

class ttt_update_message {
 public:
  ttt_update_message() {}
  ttt_update_message(bool playing, ttt_player_id pid, ttt_player_id cp,
                     ttt_player_id winner, const ttt_board& board)
      : playing(playing),
        player_id(pid),
        current_player(cp),
        winner(winner),
        board(board) {}

  ttt_message to_message() const {
    std::ostringstream ss;
    ss << msg_preamble();
    {
      boost::archive::text_oarchive oa(ss);
      oa << (*this);
    }

    std::string msg_data = ss.str();

    ttt_message msg;
    memcpy(msg.body(), msg_data.c_str(), msg_data.size());
    msg.body_length(msg_data.size());
    msg.encode_header();

    return msg;
  }

  static bool try_parse(const ttt_message& msg, ttt_update_message& umsg) {
    if (msg.body_length() < msg_preamble().size() + 1) {
      return false; // Message body length is not long enough
    }

    if (strncmp(msg.body(), msg_preamble().c_str(), msg_preamble().size()) !=
        0) {
      return false; // Preamble doesn't match
    }
    
    // Try to recover update message
    try {
      const unsigned object_length = msg.body_length() - msg_preamble().size();

      char buffer[object_length + 1];
      strncpy(buffer, msg.body() + msg_preamble().size(), object_length);
      buffer[object_length] = 0;

      std::stringstream ss(buffer);
      boost::archive::text_iarchive ia(ss);

      ia >> umsg;
    } catch (boost::archive::archive_exception& e) {
      return false;
    }

    return true;
  }

 public:
  bool playing;
  ttt_player_id player_id;
  ttt_player_id current_player;
  ttt_player_id winner;
  ttt_board board;
  
 private:
  friend class boost::serialization::access;
  
  static const std::string msg_preamble() {
    return "37ffb46b-5005-4b46-bbf2-d6595d1c3cb1";
  }
  
  template <class Archive>
  void serialize(Archive& ar, const unsigned int version) {
    ar & playing;
    ar & player_id;
    ar & current_player;
    ar & winner;
    ar & board;
  }
};

//----------------------------------------------------------------------

#endif  // ttt_message_HPP
