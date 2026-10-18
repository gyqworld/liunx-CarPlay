#pragma once

#include "carplay/platform/bluetooth.hpp"

#include <bluetooth/bluetooth.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace carplay {

/**
 * Linux BlueZ transport: SDP lookup for CarPlay UUID then RFCOMM,
 * matching LinuxPlay native/linux-platform.py.
 */
class BluezBluetoothTransport final : public IBluetoothTransport {
 public:
  BluezBluetoothTransport();
  ~BluezBluetoothTransport() override;

  std::vector<BluetoothDevice> list_paired() override;
  bool connect_rfcomm(const std::string& address) override;
  void disconnect() override;
  bool is_connected() const override;
  bool write(const std::uint8_t* data, std::size_t len) override;
  void set_data_handler(DataHandler handler) override;

  std::string last_error() const;

 private:
  void reader_main();
  int discover_rfcomm_channel(const bdaddr_t& addr, std::string& error);
  int connect_channel(const bdaddr_t& addr, int channel, std::string& error);

  mutable std::mutex mu_;
  DataHandler handler_;
  std::thread reader_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  int fd_ = -1;
  std::string last_error_;
  std::string address_;
};

}  // namespace carplay
