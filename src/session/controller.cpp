#include "carplay/session/controller.hpp"

#include "carplay/protocol/csm_framer.hpp"
#include "carplay/protocol/tlv8.hpp"
#include "carplay/protocol/version.hpp"
#include "carplay/session/receiver.hpp"

#if defined(__linux__)
#include "carplay/platform/bluez_bluetooth.hpp"
#endif

#include <memory>

namespace carplay {
namespace {

bool valid_mac(const std::string& mac) {
  if (mac.size() != 17) return false;
  for (std::size_t i = 0; i < mac.size(); ++i) {
    const char c = mac[i];
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool valid_wifi_secret(const RuntimeConfig& cfg) {
  if (cfg.ssid.empty() || cfg.ssid.size() > 32 || cfg.ssid.find('\0') != std::string::npos) return false;
  if (cfg.password.size() < 8 || cfg.password.size() > 63 || cfg.password.find('\0') != std::string::npos) return false;
  const int min_ch = cfg.wireless_mode == WirelessMode::Hotspot ? 0 : 1;
  return cfg.channel >= min_ch && cfg.channel <= 196;
}

}  // namespace

const char* session_state_name(SessionState state) {
  switch (state) {
    case SessionState::Idle:
      return "idle";
    case SessionState::Starting:
      return "starting";
    case SessionState::WaitingNetwork:
      return "waiting-network";
    case SessionState::Active:
      return "active";
    case SessionState::Stopping:
      return "stopping";
    case SessionState::Failed:
      return "failed";
  }
  return "unknown";
}

Controller::Controller(std::shared_ptr<IBluetoothTransport> bt, std::shared_ptr<IWifiAccessPoint> wifi,
                       std::shared_ptr<IStorage> storage)
    : bt_(std::move(bt)), wifi_(std::move(wifi)), storage_(std::move(storage)) {}

Controller::~Controller() { stop(); }

void Controller::set_config(RuntimeConfig config) { config_ = std::move(config); }

const RuntimeConfig& Controller::config() const { return config_; }

IStorage& Controller::storage() { return *storage_; }

void Controller::set_status_handler(std::function<void(const std::string&)> handler) {
  std::lock_guard<std::mutex> lock(mu_);
  on_status_ = std::move(handler);
}

void Controller::set_video_handler(std::function<void(const ByteBuffer&)> handler) {
  std::lock_guard<std::mutex> lock(mu_);
  on_video_ = std::move(handler);
}

void Controller::set_host_ui_handler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(mu_);
  on_host_ui_ = std::move(handler);
}

void Controller::send_touch(float x, float y, bool down) {
  std::lock_guard<std::mutex> lock(mu_);
  if (receiver_) receiver_->post_touch(x, y, down);
}

SessionState Controller::state() const { return state_; }

std::string Controller::last_error() const { return last_error_; }

std::string Controller::detail() const {
  std::lock_guard<std::mutex> lock(mu_);
  return detail_;
}

bool Controller::start() {
  last_error_.clear();
  detail_.clear();
  state_ = SessionState::Starting;
  if (!storage_ || !storage_->ensure_data_dir()) {
    last_error_ = "cannot create data directory";
    state_ = SessionState::Failed;
    return false;
  }
  if (!valid_wifi_secret(config_)) {
    last_error_ = "Wi-Fi name or password is invalid";
    state_ = SessionState::Failed;
    return false;
  }
  if (!valid_mac(config_.target_bluetooth)) {
    last_error_ = "Bluetooth address is invalid";
    state_ = SessionState::Failed;
    return false;
  }
  if (config_.width < 320 || config_.height < 200 || (config_.width % 2) || (config_.height % 2)) {
    last_error_ = "video size must be even and at least 320x200";
    state_ = SessionState::Failed;
    return false;
  }
  if (config_.fps != 30 && config_.fps != 60 && config_.fps != 90 && config_.fps != 120) {
    last_error_ = "fps must be 30, 60, 90, or 120";
    state_ = SessionState::Failed;
    return false;
  }
  if (config_.physical_width_mm < 0 || config_.physical_height_mm < 0 ||
      config_.physical_width_mm > 2000 || config_.physical_height_mm > 2000) {
    last_error_ = "physical size must be 0..2000 mm";
    state_ = SessionState::Failed;
    return false;
  }
  if (config_.max_bit_rate != 0 &&
      (config_.max_bit_rate < 5000000 || config_.max_bit_rate > 120000000)) {
    last_error_ = "max bitrate must be 0 (omit) or 5..120 Mbps";
    state_ = SessionState::Failed;
    return false;
  }
  if (!wifi_ || !bt_) {
    last_error_ = "platform transport is missing";
    state_ = SessionState::Failed;
    return false;
  }

  if (config_.mfi_dir.empty()) config_.mfi_dir = storage_->data_dir() + "/authentication";
  receiver_ = std::make_unique<Receiver>(bt_, wifi_);
  std::string error;
  const bool ok = receiver_->start(
      config_, error,
      [this](const std::string& status) {
        std::function<void(const std::string&)> handler;
        {
          std::lock_guard<std::mutex> lock(mu_);
          detail_ = status;
          handler = on_status_;
        }
        if (handler) handler(status);
      },
      [this](const ByteBuffer& annexb) {
        std::function<void(const ByteBuffer&)> handler;
        {
          std::lock_guard<std::mutex> lock(mu_);
          handler = on_video_;
        }
        if (handler) handler(annexb);
      },
      [this] {
        std::function<void()> handler;
        {
          std::lock_guard<std::mutex> lock(mu_);
          handler = on_host_ui_;
        }
        if (handler) handler();
      });
  if (!ok) {
    last_error_ = error.empty() ? "receiver failed" : error;
    state_ = SessionState::Failed;
    receiver_.reset();
    return false;
  }
  state_ = SessionState::WaitingNetwork;
  return true;
}

void Controller::stop() {
  std::unique_ptr<Receiver> doomed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    doomed = std::move(receiver_);
  }
  if (doomed) doomed->stop();
  if (bt_) bt_->disconnect();
  state_ = SessionState::Idle;
  last_error_.clear();
}

DoctorReport Controller::doctor() const {
  DoctorReport report;
  report.library_version = library_version();

  CsmFramer::Packet packet;
  packet.control = 0x40;
  packet.seq = 1;
  packet.ack = 0;
  packet.session_id = 1;
  packet.payload = {0x01, 0x02, 0x03};
  auto encoded = CsmFramer::encode(packet);
  auto decoded = CsmFramer::try_decode(encoded);
  if (!decoded || decoded->payload != packet.payload || !encoded.empty()) {
    report.detail = "CSM framer roundtrip failed";
    return report;
  }

  const std::vector<Tlv8> items = {{0x01, {0xaa, 0xbb}}, {0x02, {}}};
  const auto tlv = Tlv8Codec::encode(items);
  const auto back = Tlv8Codec::decode(tlv);
  if (back.size() != 2 || back[0].value.size() != 2 || !back[1].value.empty()) {
    report.detail = "TLV8 roundtrip failed";
    return report;
  }

  report.ok = true;
  report.detail = "CSM and TLV8 self-test passed";
  return report;
}

std::unique_ptr<Controller> make_default_controller() {
#if defined(__linux__)
  auto bt = std::shared_ptr<IBluetoothTransport>(std::make_shared<BluezBluetoothTransport>());
#else
  auto bt = std::shared_ptr<IBluetoothTransport>(std::make_shared<LoopbackBluetoothTransport>());
#endif
  return std::make_unique<Controller>(std::move(bt), std::make_shared<ManualWifiAccessPoint>(),
                                      std::make_shared<PosixStorage>());
}

}  // namespace carplay
