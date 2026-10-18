#include "carplay/platform/system_info.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <regex>
#include <sstream>
#include <vector>

namespace carplay {
namespace {

std::string run_command(const std::string& command) {
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
  if (!pipe) return {};
  std::string out;
  std::array<char, 512> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe.get())) out += buf.data();
  return out;
}

std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) value.pop_back();
  std::size_t i = 0;
  while (i < value.size() && value[i] == ' ') ++i;
  return value.substr(i);
}

std::string upper_mac(std::string mac) {
  for (char& c : mac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return mac;
}

bool looks_like_mac(const std::string& value) {
  static const std::regex re("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$");
  return std::regex_match(value, re);
}

std::string field_after(const std::string& line, char sep = ':') {
  const auto pos = line.find(sep);
  if (pos == std::string::npos) return {};
  return trim(line.substr(pos + 1));
}

struct ActiveWifi {
  std::string uuid;
  std::string name;
  std::string device;
  std::string mode;
  std::string ssid;
  int channel = 0;
};

std::vector<ActiveWifi> active_wifi_profiles() {
  const std::string text =
      run_command("LC_ALL=C.UTF-8 nmcli -t --fields UUID,TYPE,ACTIVE,DEVICE connection show --active 2>/dev/null");
  std::vector<ActiveWifi> profiles;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    // uuid:type:active:device — type may contain escaped colons rarely; keep simple split.
    std::vector<std::string> parts;
    std::string part;
    for (char c : line) {
      if (c == ':') {
        parts.push_back(part);
        part.clear();
      } else if (c == '\\') {
        continue;
      } else {
        part.push_back(c);
      }
    }
    parts.push_back(part);
    if (parts.size() < 4) continue;
    if (parts[1] != "802-11-wireless" && parts[1] != "wifi") continue;
    if (parts[2] != "yes" && parts[2] != "是") continue;
    ActiveWifi wifi;
    wifi.uuid = parts[0];
    wifi.device = parts[3];
    const std::string detail = run_command(
        "LC_ALL=C.UTF-8 nmcli -t --fields 802-11-wireless.ssid,802-11-wireless.mode,802-11-wireless.channel,connection.id "
        "connection show uuid " +
        wifi.uuid + " 2>/dev/null");
    std::istringstream detail_stream(detail);
    std::string dline;
    while (std::getline(detail_stream, dline)) {
      if (dline.rfind("802-11-wireless.ssid:", 0) == 0) wifi.ssid = field_after(dline);
      if (dline.rfind("802-11-wireless.mode:", 0) == 0) wifi.mode = field_after(dline);
      if (dline.rfind("802-11-wireless.channel:", 0) == 0) wifi.channel = std::atoi(field_after(dline).c_str());
      if (dline.rfind("connection.id:", 0) == 0) wifi.name = field_after(dline);
    }
    profiles.push_back(wifi);
  }
  return profiles;
}

std::string read_psk(const std::string& uuid) {
  const std::string text = run_command(
      "LC_ALL=C.UTF-8 nmcli --show-secrets -t --fields 802-11-wireless-security.key-mgmt,802-11-wireless-security.psk "
      "connection show uuid " +
      uuid + " 2>/dev/null");
  std::string kind;
  std::string psk;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind("802-11-wireless-security.key-mgmt:", 0) == 0) kind = field_after(line);
    if (line.rfind("802-11-wireless-security.psk:", 0) == 0) psk = field_after(line);
  }
  if ((kind != "wpa-psk" && kind != "sae") || psk.empty() || psk == "--") return {};
  return psk;
}

void fill_bssid_channel(CurrentNetworkInfo& info) {
  if (info.interface_name.empty() || info.ssid.empty()) return;
  const std::string text = run_command("LC_ALL=C.UTF-8 nmcli -t --fields IN-USE,SSID,CHAN,BSSID device wifi list ifname " +
                                       info.interface_name + " --rescan no 2>/dev/null");
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    std::vector<std::string> parts;
    std::string part;
    bool escape = false;
    for (char c : line) {
      if (escape) {
        part.push_back(c);
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == ':') {
        parts.push_back(part);
        part.clear();
      } else {
        part.push_back(c);
      }
    }
    parts.push_back(part);
    if (parts.size() < 4) continue;
    if (parts[0] != "*" || parts[1] != info.ssid) continue;
    const int channel = std::atoi(parts[2].c_str());
    if (channel >= 1 && channel <= 196) info.channel = channel;
    if (looks_like_mac(parts[3])) info.bssid = upper_mac(parts[3]);
    break;
  }
}

CurrentNetworkInfo read_lan(const std::string& preferred_interface) {
  CurrentNetworkInfo info;
  info.mode = WirelessMode::Lan;
  auto profiles = active_wifi_profiles();
  ActiveWifi* selected = nullptr;
  for (auto& profile : profiles) {
    if (profile.mode == "ap" || profile.mode == "mesh" || profile.mode == "adhoc") continue;
    if (!preferred_interface.empty() && profile.device != preferred_interface) continue;
    selected = &profile;
    break;
  }
  if (!selected) {
    info.error = "未检测到已连接的 Wi-Fi，请先连接无线网络";
    return info;
  }
  if (selected->ssid.empty()) {
    info.error = "无法读取当前 Wi-Fi 名称";
    return info;
  }
  info.ok = true;
  info.ssid = selected->ssid;
  info.interface_name = selected->device;
  info.channel = selected->channel;
  info.password = read_psk(selected->uuid);
  info.password_read = !info.password.empty();
  fill_bssid_channel(info);
  if (!info.password_read) info.warning = "已读取网络，但无法读取已保存密码，请手动填写";
  if (info.channel < 1) info.warning = (info.warning.empty() ? "" : info.warning + "；") + "无法确认信道，请核对";
  return info;
}

CurrentNetworkInfo read_hotspot(const std::string& preferred_interface) {
  CurrentNetworkInfo info;
  info.mode = WirelessMode::Hotspot;
  const std::string text =
      run_command("LC_ALL=C.UTF-8 nmcli -t --fields UUID,TYPE,ACTIVE,DEVICE connection show 2>/dev/null");
  std::istringstream stream(text);
  std::string line;
  std::vector<ActiveWifi> hotspots;
  int active_index = -1;
  while (std::getline(stream, line)) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : line) {
      if (c == ':') {
        parts.push_back(part);
        part.clear();
      } else {
        part.push_back(c);
      }
    }
    parts.push_back(part);
    if (parts.size() < 4) continue;
    if (parts[1] != "802-11-wireless" && parts[1] != "wifi") continue;
    ActiveWifi wifi;
    wifi.uuid = parts[0];
    wifi.device = parts[3];
    const bool active = parts[2] == "yes" || parts[2] == "是";
    const std::string detail = run_command(
        "LC_ALL=C.UTF-8 nmcli -t --fields 802-11-wireless.ssid,802-11-wireless.mode,802-11-wireless.channel,"
        "ipv4.method connection show uuid " +
        wifi.uuid + " 2>/dev/null");
    std::string method;
    std::istringstream detail_stream(detail);
    std::string dline;
    while (std::getline(detail_stream, dline)) {
      if (dline.rfind("802-11-wireless.ssid:", 0) == 0) wifi.ssid = field_after(dline);
      if (dline.rfind("802-11-wireless.mode:", 0) == 0) wifi.mode = field_after(dline);
      if (dline.rfind("802-11-wireless.channel:", 0) == 0) wifi.channel = std::atoi(field_after(dline).c_str());
      if (dline.rfind("ipv4.method:", 0) == 0) method = field_after(dline);
    }
    if (wifi.mode != "ap" || method != "shared") continue;
    if (!preferred_interface.empty() && !wifi.device.empty() && wifi.device != preferred_interface) continue;
    hotspots.push_back(wifi);
    if (active) active_index = static_cast<int>(hotspots.size()) - 1;
  }
  if (hotspots.empty()) {
    info.error = "未找到 NetworkManager 热点配置，请先在系统设置中创建 Wi-Fi 热点";
    return info;
  }
  const ActiveWifi& wifi = active_index >= 0 ? hotspots[static_cast<std::size_t>(active_index)] : hotspots.front();
  info.hotspot_state = active_index >= 0 ? "On" : "Off";
  info.ok = true;
  info.ssid = wifi.ssid;
  info.interface_name = wifi.device;
  info.channel = wifi.channel;
  info.password = read_psk(wifi.uuid);
  info.password_read = !info.password.empty();
  if (!info.password_read) info.warning = "热点名称已读取，但无法读取热点密码，请手动填写";
  if (info.hotspot_state != "On") info.warning = (info.warning.empty() ? "" : info.warning + "；") + "热点尚未开启";
  return info;
}

}  // namespace

CurrentNetworkInfo read_current_network(WirelessMode mode, const std::string& preferred_interface) {
  if (mode == WirelessMode::Hotspot) return read_hotspot(preferred_interface);
  return read_lan(preferred_interface);
}

BluetoothInventory list_bluetooth_inventory() {
  BluetoothInventory inventory;
  const std::string show = run_command("LC_ALL=C.UTF-8 bluetoothctl --timeout 5 show 2>/dev/null");
  std::smatch match;
  static const std::regex adapter_re("Controller ([0-9A-Fa-f:]{17})");
  if (!std::regex_search(show, match, adapter_re)) {
    inventory.error = "未检测到可用蓝牙适配器，请启动 bluetooth 服务";
    return inventory;
  }
  inventory.adapter = upper_mac(match[1].str());
  if (show.find("Powered: yes") == std::string::npos && show.find("Powered: 是") == std::string::npos) {
    inventory.error = "蓝牙适配器尚未开启";
    return inventory;
  }
  std::string devices_text = run_command("LC_ALL=C.UTF-8 bluetoothctl --timeout 5 devices Paired 2>/dev/null");
  if (devices_text.find("Invalid command") != std::string::npos || devices_text.empty()) {
    devices_text = run_command("LC_ALL=C.UTF-8 bluetoothctl --timeout 5 paired-devices 2>/dev/null");
  }
  static const std::regex device_re("Device ([0-9A-Fa-f:]{17}) (.+)");
  std::istringstream stream(devices_text);
  std::string line;
  while (std::getline(stream, line)) {
    std::smatch dm;
    if (!std::regex_search(line, dm, device_re)) continue;
    BluetoothDevice device;
    device.address = upper_mac(dm[1].str());
    device.name = trim(dm[2].str());
    device.paired = true;
    inventory.devices.push_back(device);
  }
  inventory.ok = true;
  return inventory;
}

std::vector<NetworkInterfaceInfo> list_network_interfaces() {
  std::vector<NetworkInterfaceInfo> items;
  const std::string text =
      run_command("LC_ALL=C.UTF-8 nmcli -t --fields DEVICE,TYPE,STATE device status 2>/dev/null");
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    std::vector<std::string> parts;
    std::string part;
    bool escape = false;
    for (char c : line) {
      if (escape) {
        part.push_back(c);
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == ':') {
        parts.push_back(part);
        part.clear();
      } else {
        part.push_back(c);
      }
    }
    parts.push_back(part);
    if (parts.size() < 3) continue;
    if (parts[0] == "lo" || parts[0].empty()) continue;
    if (parts[1] == "bt" || parts[1] == "wifi-p2p" || parts[1] == "bridge" || parts[1] == "loopback") continue;
    NetworkInterfaceInfo item;
    item.name = parts[0];
    item.wireless = parts[1] == "wifi" || parts[1] == "802-11-wireless";
    item.up = parts[2].find("connected") != std::string::npos || parts[2].find("已连接") != std::string::npos;
    const std::string detail =
        run_command("LC_ALL=C.UTF-8 nmcli -t --fields IP4.ADDRESS device show " + item.name + " 2>/dev/null");
    std::istringstream detail_stream(detail);
    std::string dline;
    while (std::getline(detail_stream, dline)) {
      if (dline.rfind("IP4.ADDRESS", 0) != 0) continue;
      item.ipv4 = field_after(dline);
      const auto slash = item.ipv4.find('/');
      if (slash != std::string::npos) item.ipv4 = item.ipv4.substr(0, slash);
      break;
    }
    items.push_back(item);
  }
  return items;
}

}  // namespace carplay
