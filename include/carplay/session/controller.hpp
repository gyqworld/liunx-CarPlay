#pragma once

#include "carplay/platform/bluetooth.hpp"
#include "carplay/platform/storage.hpp"
#include "carplay/platform/wifi.hpp"
#include "carplay/protocol/buffer_util.hpp"
#include "carplay/session/runtime_config.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace carplay {

enum class SessionState {
  Idle,
  Starting,
  WaitingNetwork,
  Active,
  Stopping,
  Failed,
};

const char* session_state_name(SessionState state);

struct DoctorReport {
  bool ok = false;
  std::string library_version;
  std::string detail;
};

class Receiver;

/**
 * Wireless CarPlay session. start() follows the LinuxPlay receiver:
 * Wi-Fi parameters, TCP 17000, Bluetooth RFCOMM, then the iAP2 accessory.
 */
class Controller {
 public:
  Controller(std::shared_ptr<IBluetoothTransport> bt,
              std::shared_ptr<IWifiAccessPoint> wifi,
              std::shared_ptr<IStorage> storage);
  ~Controller();

  void set_config(RuntimeConfig config);
  const RuntimeConfig& config() const;
  IStorage& storage();
  void set_status_handler(std::function<void(const std::string&)> handler);
  /** Annex-B H.264 access units from the main screen stream. */
  void set_video_handler(std::function<void(const ByteBuffer&)> handler);
  /** CarPlay dock vehicle button asked the accessory to show host UI. */
  void set_host_ui_handler(std::function<void()> handler);
  /** Normalised 0..1 pointer on the video window. */
  void send_touch(float x, float y, bool down);

  SessionState state() const;
  std::string last_error() const;
  /** Short progress note after a successful start(). */
  std::string detail() const;

  /**
   * Publish Wi-Fi, listen on TCP 17000, connect RFCOMM and send the iAP2 detect marker.
   * Returns once the receiver thread is running.
   */
  bool start();
  void stop();

  /** Offline self-check — no iPhone required. */
  DoctorReport doctor() const;

 private:
  std::shared_ptr<IBluetoothTransport> bt_;
  std::shared_ptr<IWifiAccessPoint> wifi_;
  std::shared_ptr<IStorage> storage_;
  RuntimeConfig config_;
  SessionState state_ = SessionState::Idle;
  std::string last_error_;
  std::string detail_;
  std::function<void(const std::string&)> on_status_;
  std::function<void(const ByteBuffer&)> on_video_;
  std::function<void()> on_host_ui_;
  std::unique_ptr<Receiver> receiver_;
  mutable std::mutex mu_;
};

std::unique_ptr<Controller> make_default_controller();

}  // namespace carplay
