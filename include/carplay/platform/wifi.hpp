#pragma once

#include <string>

namespace carplay {

struct WifiAccessPointInfo {
  std::string ssid;
  std::string password;
  int channel = 0;
  std::string interface_name;
  bool ready = false;
};

/**
 * SoftAP / shared Wi-Fi used for wireless CarPlay handoff.
 * Desktop: NetworkManager / hostapd. Embedded: vendor WifiControl.
 */
class IWifiAccessPoint {
 public:
  virtual ~IWifiAccessPoint() = default;
  virtual WifiAccessPointInfo query() = 0;
  /** Publish the accessory SoftAP / LAN parameters. Default adapters refuse. */
  virtual bool apply(const WifiAccessPointInfo& info) {
    (void)info;
    return false;
  }
};

/** Holds the Wi-Fi parameters the session will hand to the phone. */
class ManualWifiAccessPoint final : public IWifiAccessPoint {
 public:
  WifiAccessPointInfo query() override;
  bool apply(const WifiAccessPointInfo& info) override;

 private:
  WifiAccessPointInfo info_;
};

class NullWifiAccessPoint final : public IWifiAccessPoint {
 public:
  WifiAccessPointInfo query() override;
};

}  // namespace carplay
