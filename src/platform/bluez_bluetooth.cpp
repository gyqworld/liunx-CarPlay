#include "carplay/platform/bluez_bluetooth.hpp"

#include "carplay/platform/system_info.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/rfcomm.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <unistd.h>

#include <stdexcept>
#include <vector>

namespace carplay {
namespace {

constexpr std::uint8_t kCarPlayUuid[16] = {0x00, 0x00, 0x00, 0x00, 0xde, 0xca, 0xfa, 0xde,
                                           0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xfe};

struct SdpElement {
  int kind = 0;
  std::uint64_t number = 0;
  bool is_uuid = false;
  std::vector<SdpElement> children;
};

bool decode_element(const std::uint8_t* data, std::size_t size, std::size_t& offset, int depth, SdpElement& out) {
  if (depth > 16 || offset >= size) return false;
  const std::uint8_t header = data[offset++];
  const int kind = header >> 3;
  const int size_index = header & 7;
  std::size_t length = 0;
  if (size_index < 5) {
    length = kind == 0 ? 0 : (1u << size_index);
  } else {
    const std::size_t count = 1u << (size_index - 5);
    if (offset + count > size) return false;
    for (std::size_t i = 0; i < count; ++i) length = (length << 8) | data[offset++];
  }
  if (offset + length > size) return false;
  const std::size_t end = offset + length;
  out.kind = kind;
  if (kind == 6 || kind == 7) {
    while (offset < end) {
      SdpElement child;
      if (!decode_element(data, size, offset, depth + 1, child)) return false;
      out.children.push_back(std::move(child));
    }
    return offset == end;
  }
  if (kind == 3) {
    out.is_uuid = true;
    for (std::size_t i = offset; i < end; ++i) out.number = (out.number << 8) | data[i];
  } else if (kind == 1 || kind == 2) {
    for (std::size_t i = offset; i < end; ++i) out.number = (out.number << 8) | data[i];
  }
  offset = end;
  return true;
}

int channel_from_attributes(const std::vector<std::uint8_t>& attributes) {
  std::size_t offset = 0;
  SdpElement root;
  if (!decode_element(attributes.data(), attributes.size(), offset, 0, root) || offset != attributes.size()) {
    throw std::runtime_error("SDP服务响应无效");
  }
  if (root.kind != 6 && root.kind != 7) throw std::runtime_error("SDP服务响应无效");
  for (const auto& record : root.children) {
    if (record.kind != 6 && record.kind != 7) continue;
    for (std::size_t i = 0; i + 1 < record.children.size(); i += 2) {
      const auto& key = record.children[i];
      const auto& value = record.children[i + 1];
      if (key.number != 4 || (value.kind != 6 && value.kind != 7)) continue;
      for (const auto& protocol : value.children) {
        if ((protocol.kind != 6 && protocol.kind != 7) || protocol.children.size() < 2) continue;
        const auto& uuid = protocol.children[0];
        const auto& channel = protocol.children[1];
        if (uuid.is_uuid && uuid.number == 3 && channel.number >= 1 && channel.number <= 30) {
          return static_cast<int>(channel.number);
        }
      }
    }
  }
  throw std::runtime_error("iPhone未公布CarPlay蓝牙服务，请解锁手机并确认两端蓝牙配对");
}

}  // namespace

BluezBluetoothTransport::BluezBluetoothTransport() = default;

BluezBluetoothTransport::~BluezBluetoothTransport() { disconnect(); }

std::vector<BluetoothDevice> BluezBluetoothTransport::list_paired() {
  const auto inventory = list_bluetooth_inventory();
  if (!inventory.ok) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = inventory.error;
    return {};
  }
  return inventory.devices;
}

int BluezBluetoothTransport::discover_rfcomm_channel(const bdaddr_t& addr, std::string& error) {
  const int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
  if (fd < 0) {
    error = "无法创建蓝牙 L2CAP 套接字";
    return -1;
  }
  sockaddr_l2 address{};
  address.l2_family = AF_BLUETOOTH;
  address.l2_psm = htobs(1);
  address.l2_bdaddr = addr;
  timeval timeout{};
  timeout.tv_sec = 12;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  // Wake a paired iPhone before SDP; ignore failure if already connected.
  {
    char mac[18] = {};
    ba2str(&addr, mac);
    const std::string wake = std::string("bluetoothctl --timeout 3 connect ") + mac + " >/dev/null 2>&1 || true";
    std::system(wake.c_str());
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error = std::string("蓝牙 SDP 连接失败（") + std::strerror(errno) +
            "），请解锁 iPhone、打开蓝牙并靠近电脑后重试";
    ::close(fd);
    return -1;
  }

  // ServiceSearchAttributeRequest: UUID128 + ProtocolDescriptorList(0x0004).
  std::vector<std::uint8_t> request = {0x35, 0x11, 0x1c};
  request.insert(request.end(), kCarPlayUuid, kCarPlayUuid + 16);
  const std::uint8_t tail[] = {0xff, 0xff, 0x35, 0x03, 0x09, 0x00, 0x04};
  request.insert(request.end(), tail, tail + sizeof(tail));

  std::vector<std::uint8_t> attributes;
  std::vector<std::uint8_t> continuation;
  try {
    for (std::uint16_t transaction = 1; transaction <= 32; ++transaction) {
      std::vector<std::uint8_t> params = request;
      params.push_back(static_cast<std::uint8_t>(continuation.size()));
      params.insert(params.end(), continuation.begin(), continuation.end());
      std::uint8_t header[5] = {6, static_cast<std::uint8_t>((transaction >> 8) & 0xff),
                                static_cast<std::uint8_t>(transaction & 0xff),
                                static_cast<std::uint8_t>((params.size() >> 8) & 0xff),
                                static_cast<std::uint8_t>(params.size() & 0xff)};
      std::vector<std::uint8_t> packet(header, header + 5);
      packet.insert(packet.end(), params.begin(), params.end());
      if (::send(fd, packet.data(), packet.size(), 0) < 0) throw std::runtime_error("SDP 发送失败");

      std::uint8_t response[65540];
      const ssize_t n = ::recv(fd, response, sizeof(response), 0);
      if (n < 5) throw std::runtime_error("SDP响应过短");
      const std::uint8_t kind = response[0];
      const std::uint16_t reply_id = static_cast<std::uint16_t>((response[1] << 8) | response[2]);
      const std::uint16_t length = static_cast<std::uint16_t>((response[3] << 8) | response[4]);
      if (reply_id != transaction || length != static_cast<std::uint16_t>(n - 5)) {
        throw std::runtime_error("SDP响应标识或长度无效");
      }
      if (kind == 1) throw std::runtime_error("iPhone拒绝CarPlay蓝牙服务查询");
      if (kind != 7 || length < 3) throw std::runtime_error("SDP响应类型无效");
      const std::uint16_t count = static_cast<std::uint16_t>((response[5] << 8) | response[6]);
      const std::size_t boundary = 7 + count;
      if (boundary >= static_cast<std::size_t>(n)) throw std::runtime_error("SDP属性长度无效");
      attributes.insert(attributes.end(), response + 7, response + boundary);
      const std::uint8_t continuation_length = response[boundary];
      if (continuation_length > 16 || boundary + 1 + continuation_length != static_cast<std::size_t>(n)) {
        throw std::runtime_error("SDP续传状态无效");
      }
      continuation.assign(response + boundary + 1, response + n);
      if (attributes.size() > 1024 * 1024) throw std::runtime_error("SDP响应过大");
      if (continuation.empty()) {
        const int channel = channel_from_attributes(attributes);
        ::close(fd);
        return channel;
      }
    }
    throw std::runtime_error("SDP服务查询续传次数过多");
  } catch (const std::exception& ex) {
    error = ex.what();
    ::close(fd);
    return -1;
  }
}

int BluezBluetoothTransport::connect_channel(const bdaddr_t& addr, int channel, std::string& error) {
  const int fd = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (fd < 0) {
    error = "无法创建 RFCOMM 套接字";
    return -1;
  }
  sockaddr_rc address{};
  address.rc_family = AF_BLUETOOTH;
  address.rc_channel = static_cast<std::uint8_t>(channel);
  address.rc_bdaddr = addr;
  timeval timeout{};
  timeout.tv_sec = 15;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error = std::string("蓝牙 RFCOMM 连接失败：") + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  timeout.tv_sec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  return fd;
}

bool BluezBluetoothTransport::connect_rfcomm(const std::string& address) {
  disconnect();
  bdaddr_t mac{};
  if (str2ba(address.c_str(), &mac) != 0) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = "蓝牙地址格式无效";
    return false;
  }
  std::string error;
  const int channel = discover_rfcomm_channel(mac, error);
  if (channel < 0) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = error;
    return false;
  }
  const int fd = connect_channel(mac, channel, error);
  if (fd < 0) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = error;
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd_ = fd;
    address_ = address;
    last_error_.clear();
    stop_ = false;
    connected_ = true;
  }
  reader_ = std::thread([this] { reader_main(); });
  return true;
}

void BluezBluetoothTransport::reader_main() {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd = fd_;
  }
  std::uint8_t buf[4096];
  while (!stop_ && fd >= 0) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, 200);
    if (ready < 0) break;
    if (ready == 0) continue;
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    DataHandler handler;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handler = handler_;
    }
    if (handler) handler(buf, static_cast<std::size_t>(n));
  }
  connected_ = false;
}

void BluezBluetoothTransport::disconnect() {
  stop_ = true;
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd = fd_;
    fd_ = -1;
  }
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  if (reader_.joinable()) reader_.join();
  connected_ = false;
}

bool BluezBluetoothTransport::is_connected() const { return connected_; }

bool BluezBluetoothTransport::write(const std::uint8_t* data, std::size_t len) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd = fd_;
  }
  if (fd < 0 || data == nullptr || len == 0) return false;
  std::size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(fd, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void BluezBluetoothTransport::set_data_handler(DataHandler handler) {
  std::lock_guard<std::mutex> lock(mu_);
  handler_ = std::move(handler);
}

std::string BluezBluetoothTransport::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

}  // namespace carplay
