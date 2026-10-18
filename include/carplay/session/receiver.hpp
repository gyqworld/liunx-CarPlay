#pragma once

#include "carplay/platform/bluetooth.hpp"
#include "carplay/platform/wifi.hpp"
#include "carplay/protocol/iap_accessory.hpp"
#include "carplay/protocol/mfi_signer.hpp"
#include "carplay/session/control_session.hpp"
#include "carplay/session/runtime_config.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace carplay {

struct ControlClient {
  int fd = -1;
  ControlSession session;
};

/**
 * Wireless receiver pipeline aligned with LinuxPlay's engine/run.ts begin():
 * TCP 17000, RFCOMM, iAP2 accessory, then wait for the phone's control connection.
 */
class Receiver {
 public:
  using StatusHandler = std::function<void(const std::string&)>;
  using FrameHandler = ScreenPort::FrameHandler;
  using HostUiHandler = std::function<void()>;

  Receiver(std::shared_ptr<IBluetoothTransport> bt, std::shared_ptr<IWifiAccessPoint> wifi);
  ~Receiver();

  bool start(const RuntimeConfig& config, std::string& error, StatusHandler status, FrameHandler frames = {},
             HostUiHandler host_ui = {});
  void stop();
  /** Normalised 0..1 touch, safe from the UI thread. */
  void post_touch(float x, float y, bool down);

 private:
  void thread_main();
  void publish(const std::string& status);
  bool listen_port(std::string& error);
  bool read_interface(std::string& error);
  void reset_local();

  std::shared_ptr<IBluetoothTransport> bt_;
  std::shared_ptr<IWifiAccessPoint> wifi_;
  RuntimeConfig config_;
  StatusHandler status_;
  FrameHandler on_frame_;
  HostUiHandler on_host_ui_;
  Handoff handoff_;
  std::unique_ptr<IMfiSigner> signer_;
  std::unique_ptr<IapLink> link_;
  std::unique_ptr<Accessory> accessory_;
  std::mutex status_mu_;
  std::mutex link_mu_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> control_count_{0};
  int listen_fd_ = -1;
  int wake_r_ = -1;
  int wake_w_ = -1;
  std::mutex touch_mu_;
  bool accepting_touch_ = false;
  struct TouchPoint {
    float x = 0;
    float y = 0;
    bool down = false;
  };
  std::vector<TouchPoint> touches_;
  std::vector<std::unique_ptr<ControlClient>> clients_;
};

}  // namespace carplay
