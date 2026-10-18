#pragma once

#include "carplay/protocol/pair_setup.hpp"
#include "carplay/protocol/pair_verify.hpp"
#include "carplay/session/runtime_config.hpp"
#include "carplay/session/session_services.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace carplay {

/** AirPlay/CarPlay control responder on TCP 17000. */
class ControlSession {
 public:
  ControlSession() = default;
  ~ControlSession();
  ControlSession(const ControlSession&) = delete;
  ControlSession& operator=(const ControlSession&) = delete;

  /** Feed socket bytes; returns responses to write (may be empty). */
  std::vector<std::string> feed(const std::uint8_t* data, std::size_t len);

  /** Last request line, for status logging. */
  const std::string& last_request_line() const { return last_line_; }

  void set_signer(IMfiSigner* signer) { signer_ = signer; }
  void set_config(const RuntimeConfig& config) { config_ = config; }
  void set_peer_host(std::string host) { peer_host_ = std::move(host); }
  void set_status_handler(std::function<void(const std::string&)> handler) { status_ = std::move(handler); }
  void set_frame_handler(ScreenPort::FrameHandler handler) { on_frame_ = std::move(handler); }
  /** CarPlay dock "vehicle" button → phone sends requestUI (no url). */
  void set_host_ui_handler(std::function<void()> handler) { on_host_ui_ = std::move(handler); }
  /** Normalised 0..1 coordinates on the main screen. */
  void send_touch(float x, float y, bool down);

 private:
  std::string handle_message(const std::string& message);
  std::string handle_setup(const std::string& protocol, const std::string& cseq, const std::string& body);
  std::string handle_info(const std::string& protocol, const std::string& cseq);
  std::string handle_feedback(const std::string& protocol, const std::string& cseq);
  std::string handle_command(const std::string& protocol, const std::string& cseq, const std::string& body);
  void notify_host_ui(const std::string& reason);
  void request_keyframe();
  void stop_stream(int type);
  static std::string http_response(int status, const std::string& content_type, const std::string& body,
                                   const std::string& extra_headers = {});
  static std::string rtsp_response(int status, const std::string& cseq, const std::string& extra = {});
  std::string reply(const std::string& protocol, int status, const std::string& reason, const std::string& cseq,
                    const std::string& content_type, const std::string& body) const;

  PairSetup pair_;
  PairVerify verify_;
  std::optional<ControlCipher> cipher_;
  IMfiSigner* signer_ = nullptr;
  RuntimeConfig config_;
  std::string peer_host_;
  std::unique_ptr<TimingSync> timing_;
  std::unique_ptr<TimingSync> keep_alive_;
  std::unique_ptr<EventChannel> events_;
  std::function<void(const std::string&)> status_;
  ScreenPort::FrameHandler on_frame_;
  std::function<void()> on_host_ui_;
  std::unique_ptr<ScreenPort> screen_;
  std::vector<std::unique_ptr<AudioPort>> audios_;
  std::string enc_;
  std::string buffer_;
  std::string last_line_;
};

}  // namespace carplay
