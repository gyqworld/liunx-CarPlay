#pragma once

#include "carplay/platform/bluetooth.hpp"
#include "carplay/session/runtime_config.hpp"

#include <string>
#include <vector>

namespace carplay {

struct CurrentNetworkInfo {
  bool ok = false;
  WirelessMode mode = WirelessMode::Lan;
  std::string ssid;
  std::string password;
  bool password_read = false;
  int channel = 0;
  std::string bssid;
  std::string interface_name;
  std::string hotspot_state;  // On / Off / InTransition
  std::string error;
  std::string warning;
};

struct BluetoothInventory {
  bool ok = false;
  std::string adapter;
  std::vector<BluetoothDevice> devices;
  std::string error;
};

struct NetworkInterfaceInfo {
  std::string name;
  bool wireless = false;
  bool up = false;
  std::string ipv4;
};

/** Read active LAN Wi-Fi or SoftAP settings via NetworkManager (nmcli). */
CurrentNetworkInfo read_current_network(WirelessMode mode, const std::string& preferred_interface = {});

/** List Bluetooth adapter + paired devices via bluetoothctl / BlueZ. */
BluetoothInventory list_bluetooth_inventory();

std::vector<NetworkInterfaceInfo> list_network_interfaces();

}  // namespace carplay
