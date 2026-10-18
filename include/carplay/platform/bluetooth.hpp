#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace carplay {

struct BluetoothDevice {
  std::string address;  // "AA:BB:CC:DD:EE:FF"
  std::string name;
  bool paired = false;
};

/**
 * RFCOMM / accessory Bluetooth transport.
 * Desktop: BlueZ. Embedded (e.g. Sunplus 8368): vendor BT stack adapter.
 */
class IBluetoothTransport {
 public:
  virtual ~IBluetoothTransport() = default;

  virtual std::vector<BluetoothDevice> list_paired() = 0;
  virtual bool connect_rfcomm(const std::string& address) = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;

  virtual bool write(const std::uint8_t* data, std::size_t len) = 0;
  using DataHandler = std::function<void(const std::uint8_t*, std::size_t)>;
  virtual void set_data_handler(DataHandler handler) = 0;
  virtual std::string last_error() const { return {}; }
};

/**
 * In-process RFCOMM stand-in used until BlueZ or the 8368 stack is attached.
 * connect_rfcomm() succeeds for a non-empty address and write() keeps the bytes.
 */
class LoopbackBluetoothTransport final : public IBluetoothTransport {
 public:
  std::vector<BluetoothDevice> list_paired() override;
  bool connect_rfcomm(const std::string& address) override;
  void disconnect() override;
  bool is_connected() const override;
  bool write(const std::uint8_t* data, std::size_t len) override;
  void set_data_handler(DataHandler handler) override;

  const std::vector<std::uint8_t>& written() const { return written_; }
  void inject(const std::uint8_t* data, std::size_t len);

 private:
  DataHandler handler_;
  bool connected_ = false;
  std::string address_;
  std::vector<std::uint8_t> written_;
};

/** Placeholder transport — always empty / disconnected. */
class NullBluetoothTransport final : public IBluetoothTransport {
 public:
  std::vector<BluetoothDevice> list_paired() override;
  bool connect_rfcomm(const std::string& address) override;
  void disconnect() override;
  bool is_connected() const override;
  bool write(const std::uint8_t* data, std::size_t len) override;
  void set_data_handler(DataHandler handler) override;

 private:
  DataHandler handler_;
  bool connected_ = false;
};

}  // namespace carplay
