#include "carplay/platform/wifi.hpp"

namespace carplay {

WifiAccessPointInfo NullWifiAccessPoint::query() {
  WifiAccessPointInfo info;
  info.ready = false;
  return info;
}

WifiAccessPointInfo ManualWifiAccessPoint::query() { return info_; }

bool ManualWifiAccessPoint::apply(const WifiAccessPointInfo& info) {
  if (info.ssid.empty()) return false;
  info_ = info;
  info_.ready = true;
  return true;
}

}  // namespace carplay
