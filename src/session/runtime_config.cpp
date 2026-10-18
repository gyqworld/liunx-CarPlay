#include "carplay/session/runtime_config.hpp"

namespace carplay {

std::string wireless_mode_name(WirelessMode mode) {
  switch (mode) {
    case WirelessMode::Hotspot:
      return "hotspot";
    case WirelessMode::Lan:
      return "lan";
  }
  return "unknown";
}

}  // namespace carplay
