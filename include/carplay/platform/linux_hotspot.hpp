#pragma once

#include <string>

namespace carplay {

struct HotspotRequest {
  std::string iface = "wlo1";
  std::string ssid;
  std::string password;
  /** 0 = follow the station channel (alongside mode). 2.4 GHz takeover uses 1/6/11. */
  int channel = 6;
  std::string bt_mac;
  /** Keep the current Wi-Fi client link and add a same-channel AP (LAN bitrate path). */
  bool keep_station = false;
};

struct HotspotStatus {
  bool running = false;
  std::string detail;
};

/** Start SoftAP with Apple Device IE so iPhone raises encode bitrate above ~1 Mbps. */
bool start_carplay_hotspot(const HotspotRequest& req, std::string& error);
bool stop_carplay_hotspot(std::string& error);
HotspotStatus query_carplay_hotspot();

}  // namespace carplay
