#include "carplay/session/settings_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace carplay {
namespace {

std::string escape(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

bool extract_string(const std::string& body, const char* key, std::string& out) {
  const std::string needle = std::string("\"") + key + "\"";
  const auto pos = body.find(needle);
  if (pos == std::string::npos) return false;
  auto colon = body.find(':', pos + needle.size());
  if (colon == std::string::npos) return false;
  auto quote = body.find('"', colon + 1);
  if (quote == std::string::npos) return false;
  std::string value;
  for (std::size_t i = quote + 1; i < body.size(); ++i) {
    if (body[i] == '\\' && i + 1 < body.size()) {
      value.push_back(body[++i]);
      continue;
    }
    if (body[i] == '"') {
      out = value;
      return true;
    }
    value.push_back(body[i]);
  }
  return false;
}

bool extract_int(const std::string& body, const char* key, int& out) {
  const std::string needle = std::string("\"") + key + "\"";
  const auto pos = body.find(needle);
  if (pos == std::string::npos) return false;
  auto colon = body.find(':', pos + needle.size());
  if (colon == std::string::npos) return false;
  out = std::atoi(body.c_str() + colon + 1);
  return true;
}

}  // namespace

bool save_runtime_config(const IStorage& storage, const RuntimeConfig& config) {
  if (!storage.ensure_data_dir()) return false;
  const std::string path = storage.data_dir() + "/settings.json";
  const std::string tmp = path + ".new";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "{\n"
      << "  \"wirelessMode\": " << escape(wireless_mode_name(config.wireless_mode)) << ",\n"
      << "  \"ssid\": " << escape(config.ssid) << ",\n"
      << "  \"password\": " << escape(config.password) << ",\n"
      << "  \"channel\": " << config.channel << ",\n"
      << "  \"networkInterface\": " << escape(config.network_interface) << ",\n"
      << "  \"targetBluetooth\": " << escape(config.target_bluetooth) << ",\n"
      << "  \"adapterBluetooth\": " << escape(config.adapter_bluetooth) << ",\n"
      << "  \"accessPointMac\": " << escape(config.access_point_mac) << ",\n"
      << "  \"port\": " << config.port << ",\n"
      << "  \"width\": " << config.width << ",\n"
      << "  \"height\": " << config.height << ",\n"
      << "  \"fps\": " << config.fps << ",\n"
      << "  \"physicalWidthMm\": " << config.physical_width_mm << ",\n"
      << "  \"physicalHeightMm\": " << config.physical_height_mm << ",\n"
      << "  \"maxBitRate\": " << config.max_bit_rate << ",\n"
      << "  \"deviceName\": " << escape(config.device_name) << ",\n"
      << "  \"mfiDir\": " << escape(config.mfi_dir) << "\n"
      << "}\n";
  out.close();
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool load_runtime_config(const IStorage& storage, RuntimeConfig& config) {
  const std::string path = storage.data_dir() + "/settings.json";
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream body;
  body << in.rdbuf();
  const std::string text = body.str();
  std::string mode;
  if (extract_string(text, "wirelessMode", mode)) {
    config.wireless_mode = mode == "lan" ? WirelessMode::Lan : WirelessMode::Hotspot;
  }
  extract_string(text, "ssid", config.ssid);
  extract_string(text, "password", config.password);
  extract_int(text, "channel", config.channel);
  extract_string(text, "networkInterface", config.network_interface);
  extract_string(text, "targetBluetooth", config.target_bluetooth);
  extract_string(text, "adapterBluetooth", config.adapter_bluetooth);
  extract_string(text, "accessPointMac", config.access_point_mac);
  extract_int(text, "port", config.port);
  extract_int(text, "width", config.width);
  extract_int(text, "height", config.height);
  extract_int(text, "fps", config.fps);
  extract_int(text, "physicalWidthMm", config.physical_width_mm);
  extract_int(text, "physicalHeightMm", config.physical_height_mm);
  extract_int(text, "maxBitRate", config.max_bit_rate);
  extract_string(text, "deviceName", config.device_name);
  extract_string(text, "mfiDir", config.mfi_dir);
  return true;
}

}  // namespace carplay
