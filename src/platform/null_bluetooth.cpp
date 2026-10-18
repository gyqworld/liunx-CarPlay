#include "carplay/platform/bluetooth.hpp"

namespace carplay {

std::vector<BluetoothDevice> NullBluetoothTransport::list_paired() { return {}; }

bool NullBluetoothTransport::connect_rfcomm(const std::string&) {
  connected_ = false;
  return false;
}

void NullBluetoothTransport::disconnect() { connected_ = false; }

bool NullBluetoothTransport::is_connected() const { return connected_; }

bool NullBluetoothTransport::write(const std::uint8_t*, std::size_t) { return false; }

void NullBluetoothTransport::set_data_handler(DataHandler handler) { handler_ = std::move(handler); }

std::vector<BluetoothDevice> LoopbackBluetoothTransport::list_paired() {
  if (address_.empty()) return {};
  return {BluetoothDevice{address_, "loopback", true}};
}

bool LoopbackBluetoothTransport::connect_rfcomm(const std::string& address) {
  if (address.empty()) return false;
  address_ = address;
  connected_ = true;
  written_.clear();
  return true;
}

void LoopbackBluetoothTransport::disconnect() { connected_ = false; }

bool LoopbackBluetoothTransport::is_connected() const { return connected_; }

bool LoopbackBluetoothTransport::write(const std::uint8_t* data, std::size_t len) {
  if (!connected_ || data == nullptr) return false;
  written_.insert(written_.end(), data, data + len);
  return true;
}

void LoopbackBluetoothTransport::inject(const std::uint8_t* data, std::size_t len) {
  if (handler_ && data != nullptr) handler_(data, len);
}

void LoopbackBluetoothTransport::set_data_handler(DataHandler handler) { handler_ = std::move(handler); }

}  // namespace carplay
