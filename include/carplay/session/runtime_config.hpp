#pragma once

#include <cstdint>
#include <string>

namespace carplay {

enum class WirelessMode {
  Hotspot,
  Lan,
};

struct RuntimeConfig {
  WirelessMode wireless_mode = WirelessMode::Hotspot;
  std::string ssid;
  std::string password;
  int channel = 0;
  std::string network_interface;
  std::string target_bluetooth;  // phone, "AA:BB:CC:DD:EE:FF"
  std::string adapter_bluetooth;  // local adapter address sent in identification
  std::string access_point_mac;   // LAN BSSID; empty uses the interface MAC
  int port = 17000;
  int width = 1280;
  int height = 720;
  int fps = 30;
  /** Physical panel size in millimeters, reported to iPhone as widthPhysical/heightPhysical. */
  int physical_width_mm = 0;
  int physical_height_mm = 0;
  /** Screen stream bitrate ceiling (bps) in /info. 0 = omit (MacPlay-compatible). */
  int max_bit_rate = 0;
  std::string device_name = "专业拧瓶盖员";
  std::string mfi_dir;  // identity.pk8 + certificate.p7b
};

std::string wireless_mode_name(WirelessMode mode);

}  // namespace carplay
