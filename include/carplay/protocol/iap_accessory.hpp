#pragma once

#include "carplay/protocol/iap_link.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace carplay {

class IMfiSigner {
 public:
  virtual ~IMfiSigner() = default;
  virtual ByteBuffer certificate() = 0;
  virtual ByteBuffer sign_challenge(const ByteBuffer& challenge) = 0;
};

class MissingMfiSigner final : public IMfiSigner {
 public:
  ByteBuffer certificate() override { return {}; }
  ByteBuffer sign_challenge(const ByteBuffer&) override { return {}; }
};

struct Handoff {
  std::string name = "专业拧瓶盖员";
  std::string ssid;
  std::string password;
  int channel = 0;
  std::string bt_mac;
  std::string ap_mac;
  std::string host;
  int port = 17000;
  std::string pk;
  std::string serial;
};

ByteBuffer accessory_identification(const Handoff& handoff);

/**
 * Accessory role on an iAP2 link: identification, MFi, Wi-Fi info, CarPlay start.
 * Status strings match the LinuxPlay receiver.
 */
class Accessory {
 public:
  Accessory(IapLink& link, IMfiSigner& signer, Handoff handoff);

  bool authenticated() const { return authenticated_; }
  void start_carplay();
  void seek(std::uint32_t position_ms);
  void close();
  void tick(std::uint64_t now_ms);

  std::function<void(const std::string&)> on_status;
  std::function<void(const std::string&)> on_failed;
  std::function<void()> on_negotiation;

 private:
  void send_csm(std::uint16_t id, const std::vector<ByteBuffer>& params = {});
  void receive(std::uint16_t id, const std::map<std::uint16_t, ByteBuffer>& params);
  void send_wifi();
  static ByteBuffer cstr(const std::string& text);

  IapLink& link_;
  IMfiSigner& signer_;
  Handoff handoff_;
  bool authenticated_ = false;
  bool transport_notified_ = false;
  int pre_wifi_ = 0;
  int post_wifi_ = 0;
  std::uint64_t began_ms_ = 0;
  bool clock_started_ = false;
  enum class Stage { Identify, Authenticate, Ready } stage_ = Stage::Identify;
};

}  // namespace carplay
