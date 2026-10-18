#pragma once

#include "carplay/protocol/buffer_util.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace carplay {

struct IapParam {
  std::uint16_t id = 0;
  ByteBuffer value;
};

/** Outgoing iAP2 link packet. Header and payload checksums match LinuxPlay packet(). */
ByteBuffer iap_packet(std::uint8_t control, std::uint8_t seq, std::uint8_t ack, std::uint8_t session,
                      const ByteBuffer& payload);

ByteBuffer iap_param(std::uint16_t id, const ByteBuffer& value = {});
ByteBuffer iap_csm(std::uint16_t id, const std::vector<ByteBuffer>& params);

/**
 * iAP2 link: detect marker, SYN/ACK, then control-session messages.
 * The owner calls tick() and feed(); there is no internal thread.
 */
class IapLink {
 public:
  enum class State { Detect, Negotiate, Normal, Dead };

  struct Options {
    bool initiate = false;
    bool zero_ack = false;
  };

  using Writer = std::function<void(const std::uint8_t*, std::size_t)>;

  explicit IapLink(Writer writer);
  IapLink(Writer writer, Options options);

  void feed(const std::uint8_t* data, std::size_t len);
  void tick(std::uint64_t now_ms);
  void send(const ByteBuffer& payload, std::uint8_t session = 10);
  void close();

  State state() const { return state_; }
  const std::string& failure() const { return failure_; }

  std::function<void()> on_ready;
  std::function<void(std::uint16_t, const std::map<std::uint16_t, ByteBuffer>&)> on_csm;
  std::function<void(const ByteBuffer&)> on_file;
  std::function<void(const std::string&)> on_failed;

 private:
  void fail(const std::string& reason);
  void sync();
  void flush();
  void deliver(std::uint8_t session, const ByteBuffer& data);
  void write_packet(std::uint8_t control, std::uint8_t seq, std::uint8_t ack, std::uint8_t session,
                    const ByteBuffer& payload);

  Writer writer_;
  Options options_;
  State state_ = State::Detect;
  ByteBuffer buffer_;
  ByteBuffer csm_;
  std::uint8_t seq_ = 99;
  std::uint8_t ack_ = 0;
  int max_outgoing_ = 30;
  int max_len_ = 65535;
  int timeout_ms_ = 4000;
  int max_retries_ = 4;
  std::uint64_t began_ms_ = 0;
  std::uint64_t last_negotiate_ms_ = 0;
  bool clock_started_ = false;

  struct Pending {
    std::uint8_t seq = 0;
    std::uint8_t session = 0;
    ByteBuffer data;
    std::uint64_t time_ms = 0;
    int retries = 0;
  };
  std::vector<Pending> pending_;
  struct Queued {
    std::uint8_t session = 0;
    ByteBuffer data;
  };
  std::vector<Queued> queue_;
  std::map<std::uint8_t, Queued> out_of_order_;
  std::string failure_;
};

}  // namespace carplay
