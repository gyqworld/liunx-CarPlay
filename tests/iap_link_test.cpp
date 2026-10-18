#include "carplay/protocol/iap_accessory.hpp"
#include "carplay/protocol/iap_link.hpp"

#include <iostream>
#include <vector>

namespace {

void pump(std::vector<std::uint8_t>& a2b, std::vector<std::uint8_t>& b2a, carplay::IapLink& a, carplay::IapLink& b) {
  for (int i = 0; i < 12; ++i) {
    if (!a2b.empty()) {
      b.feed(a2b.data(), a2b.size());
      a2b.clear();
    }
    if (!b2a.empty()) {
      a.feed(b2a.data(), b2a.size());
      b2a.clear();
    }
  }
}

}  // namespace

int main() {
  using carplay::Accessory;
  using carplay::Handoff;
  using carplay::IapLink;
  using carplay::MissingMfiSigner;

  std::vector<std::uint8_t> a2b;
  std::vector<std::uint8_t> b2a;
  IapLink accessory_link([&](const std::uint8_t* data, std::size_t len) { a2b.insert(a2b.end(), data, data + len); });
  IapLink phone_link([&](const std::uint8_t* data, std::size_t len) { b2a.insert(b2a.end(), data, data + len); },
                     IapLink::Options{true, false});
  pump(a2b, b2a, accessory_link, phone_link);
  if (accessory_link.state() != IapLink::State::Normal || phone_link.state() != IapLink::State::Normal) {
    std::cerr << "link did not reach normal\n";
    return 1;
  }

  Handoff handoff;
  handoff.ssid = "demo-ap";
  handoff.password = "demo-pass";
  handoff.channel = 6;
  handoff.bt_mac = "02:00:00:00:00:02";
  handoff.ap_mac = "02:00:00:00:00:03";
  handoff.host = "192.0.2.2";
  handoff.port = 17000;
  handoff.pk = "ab";
  handoff.serial = "CARPLAYCPP";
  MissingMfiSigner signer;
  std::string status;
  Accessory accessory(accessory_link, signer, handoff);
  accessory.on_status = [&](const std::string& text) { status = text; };
  phone_link.send(carplay::iap_csm(0x1d00, {}));
  pump(a2b, b2a, accessory_link, phone_link);
  phone_link.send(carplay::iap_csm(0x1d02, {}));
  pump(a2b, b2a, accessory_link, phone_link);
  if (status != "iPhone已接受配件识别") {
    std::cerr << "unexpected status: " << status << "\n";
    return 1;
  }
  return 0;
}
