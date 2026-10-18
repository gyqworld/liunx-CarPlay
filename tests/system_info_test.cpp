#include "carplay/platform/system_info.hpp"

#include <iostream>

int main() {
  const auto lan = carplay::read_current_network(carplay::WirelessMode::Lan);
  std::cout << "lan ok=" << lan.ok << " ssid=" << lan.ssid << " ch=" << lan.channel
            << " iface=" << lan.interface_name << " bssid=" << lan.bssid
            << " pass=" << (lan.password_read ? "yes" : "no") << "\n";
  if (!lan.ok) std::cout << "lan error=" << lan.error << "\n";
  if (!lan.warning.empty()) std::cout << "lan warning=" << lan.warning << "\n";

  const auto bt = carplay::list_bluetooth_inventory();
  std::cout << "bt ok=" << bt.ok << " adapter=" << bt.adapter << " devices=" << bt.devices.size() << "\n";
  if (!bt.ok) std::cout << "bt error=" << bt.error << "\n";
  for (const auto& d : bt.devices) std::cout << "  " << d.address << " " << d.name << "\n";

  const auto ifaces = carplay::list_network_interfaces();
  std::cout << "ifaces=" << ifaces.size() << "\n";
  for (const auto& i : ifaces) {
    std::cout << "  " << i.name << " wifi=" << i.wireless << " up=" << i.up << " ip=" << i.ipv4 << "\n";
  }
  return 0;
}
