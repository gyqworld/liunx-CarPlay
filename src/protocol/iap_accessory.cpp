#include "carplay/protocol/iap_accessory.hpp"

#include <cctype>
#include <initializer_list>

namespace carplay {
namespace {

ByteBuffer u16be(std::uint16_t value) {
  ByteBuffer out(2);
  write_be16(out.data(), value);
  return out;
}

ByteBuffer u32be(std::uint32_t value) {
  ByteBuffer out(4);
  write_be32(out.data(), value);
  return out;
}

std::string mac_hex(const std::string& mac) {
  std::string out;
  for (char c : mac) {
    if (c == ':') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

ByteBuffer from_hex_mac(const std::string& mac) { return bytes_from_hex(mac_hex(mac)); }

}  // namespace

ByteBuffer Accessory::cstr(const std::string& text) {
  ByteBuffer out(text.begin(), text.end());
  out.push_back(0);
  return out;
}

ByteBuffer accessory_identification(const Handoff& handoff) {
  const std::uint16_t sent[] = {0xaa01, 0xaa03, 0x5000, 0x5002, 0x5003, 0x5200, 0x5203, 0xae00,
                                0xae02, 0x4157, 0x4159, 0x4154, 0x4156, 0x4301, 0x5703};
  const std::uint16_t received[] = {0xaa00, 0xaa02, 0xaa04, 0xaa05, 0xea00, 0xea01, 0x5001, 0x5201,
                                    0x5202, 0xae01, 0x4158, 0x4155, 0x4300, 0x4e0d, 0x4e0e, 0x5702};
  ByteBuffer sent_ids;
  ByteBuffer received_ids;
  for (auto id : sent) {
    auto bytes = u16be(id);
    sent_ids.insert(sent_ids.end(), bytes.begin(), bytes.end());
  }
  for (auto id : received) {
    auto bytes = u16be(id);
    received_ids.insert(received_ids.end(), bytes.begin(), bytes.end());
  }
  const std::string shown = handoff.name.empty() ? "专业拧瓶盖员" : handoff.name;
  const std::string labels[] = {shown, shown, shown, handoff.serial, "1.0.0", "1.0"};
  auto text = [](const std::string& value) {
    ByteBuffer out(value.begin(), value.end());
    out.push_back(0);
    return out;
  };
  std::vector<ByteBuffer> params;
  for (int i = 0; i < 6; ++i) params.push_back(iap_param(static_cast<std::uint16_t>(i), text(labels[i])));
  params.push_back(iap_param(6, sent_ids));
  params.push_back(iap_param(7, received_ids));
  params.push_back(iap_param(8, ByteBuffer{0}));
  params.push_back(iap_param(9, u16be(20)));
  params.push_back(iap_param(10, [&] {
    ByteBuffer group;
    auto a = iap_param(0, ByteBuffer{1});
    auto b = iap_param(1, text("io.github.carplaycpp"));
    auto c = iap_param(2, ByteBuffer{0});
    group.insert(group.end(), a.begin(), a.end());
    group.insert(group.end(), b.begin(), b.end());
    group.insert(group.end(), c.begin(), c.end());
    return group;
  }()));
  params.push_back(iap_param(12, text("en")));
  params.push_back(iap_param(13, text("en")));
  params.push_back(iap_param(17, [&] {
    ByteBuffer group;
    auto push = [&](ByteBuffer part) { group.insert(group.end(), part.begin(), part.end()); };
    push(iap_param(0, u16be(0)));
    push(iap_param(1, text("blue")));
    push(iap_param(2));
    push(iap_param(3, from_hex_mac(handoff.bt_mac)));
    push(iap_param(4, text("blue")));
    push(iap_param(5));
    return group;
  }()));
  params.push_back(iap_param(24, [&] {
    ByteBuffer group;
    auto push = [&](ByteBuffer part) { group.insert(group.end(), part.begin(), part.end()); };
    push(iap_param(0, u16be(1)));
    push(iap_param(1, text(handoff.ssid)));
    push(iap_param(2));
    push(iap_param(3, u16be(1)));
    push(iap_param(4));
    push(iap_param(5));
    return group;
  }()));
  return iap_csm(0x1d01, params);
}

Accessory::Accessory(IapLink& link, IMfiSigner& signer, Handoff handoff)
    : link_(link), signer_(signer), handoff_(std::move(handoff)) {
  link_.on_csm = [this](std::uint16_t id, const std::map<std::uint16_t, ByteBuffer>& params) { receive(id, params); };
  link_.on_failed = [this](const std::string& reason) {
    if (on_failed) on_failed(reason);
  };
}

void Accessory::send_csm(std::uint16_t id, const std::vector<ByteBuffer>& params) { link_.send(iap_csm(id, params)); }

void Accessory::send_wifi() {
  const int count = transport_notified_ ? post_wifi_ : pre_wifi_;
  if (count >= 3) return;
  if (transport_notified_)
    ++post_wifi_;
  else
    ++pre_wifi_;
  send_csm(0x5703, {iap_param(1, cstr(handoff_.ssid)), iap_param(2, cstr(handoff_.password)), iap_param(3, ByteBuffer{2}),
                    iap_param(4, ByteBuffer{static_cast<std::uint8_t>(handoff_.channel)})});
}

void Accessory::start_carplay() {
  if (!authenticated_) return;
  ByteBuffer group;
  auto push = [&](ByteBuffer part) { group.insert(group.end(), part.begin(), part.end()); };
  push(iap_param(0, cstr(handoff_.ssid)));
  push(iap_param(1, cstr(handoff_.password)));
  push(iap_param(2, ByteBuffer{static_cast<std::uint8_t>(handoff_.channel)}));
  push(iap_param(3, cstr(handoff_.host)));
  push(iap_param(4, ByteBuffer{2}));
  ByteBuffer port = u32be(static_cast<std::uint32_t>(handoff_.port));
  send_csm(0x4301, {iap_param(1, group), iap_param(2, port), iap_param(3, cstr(handoff_.ap_mac)),
                    iap_param(4, cstr(handoff_.pk)), iap_param(5, cstr("950.7.1"))});
  if (on_negotiation) on_negotiation();
}

void Accessory::seek(std::uint32_t position_ms) { send_csm(0x5003, {iap_param(0, u32be(position_ms))}); }

void Accessory::close() { link_.close(); }

void Accessory::tick(std::uint64_t now_ms) {
  if (!clock_started_) {
    clock_started_ = true;
    began_ms_ = now_ms;
  }
  if (!authenticated_ && now_ms - began_ms_ > 45000) {
    if (on_failed) on_failed("配件识别或认证超时");
    close();
  }
}

void Accessory::receive(std::uint16_t id, const std::map<std::uint16_t, ByteBuffer>& params) {
  if (id == 0x1d00) {
    link_.send(accessory_identification(handoff_));
    return;
  }
  if (id == 0x1d03) {
    if (on_failed) on_failed("iPhone拒绝配件识别参数");
    return;
  }
  if (id == 0x1d02) {
    stage_ = Stage::Authenticate;
    if (on_status) on_status("iPhone已接受配件识别");
    return;
  }
  if (id == 0xaa00 && stage_ == Stage::Authenticate) {
    const ByteBuffer cert = signer_.certificate();
    if (cert.empty()) {
      if (on_failed) on_failed("请先导入认证文件");
      return;
    }
    send_csm(0xaa01, {iap_param(0, cert)});
    return;
  }
  if (id == 0xaa02 && stage_ == Stage::Authenticate) {
    const auto it = params.find(0);
    if (it == params.end() || it->second.size() != 32) {
      if (on_failed) on_failed("挑战长度错误");
      return;
    }
    const ByteBuffer signature = signer_.sign_challenge(it->second);
    if (signature.empty()) {
      if (on_failed) on_failed("请先导入认证文件");
      return;
    }
    send_csm(0xaa03, {iap_param(0, signature)});
    return;
  }
  if (id == 0xaa04) {
    if (on_failed) on_failed("iPhone拒绝MFi认证材料");
    return;
  }
  if (id == 0xaa05 && stage_ == Stage::Authenticate) {
    authenticated_ = true;
    stage_ = Stage::Ready;
    if (on_status) on_status("MFi认证已通过，等待CarPlay请求");
    ByteBuffer sent_group;
    ByteBuffer recv_group;
    for (std::uint16_t n : std::initializer_list<std::uint16_t>{0, 1, 4, 6, 12, 26}) {
      auto part = iap_param(n);
      sent_group.insert(sent_group.end(), part.begin(), part.end());
    }
    for (std::uint16_t n : std::initializer_list<std::uint16_t>{0, 1, 7, 12, 13, 16}) {
      auto part = iap_param(n);
      recv_group.insert(recv_group.end(), part.begin(), part.end());
    }
    send_csm(0x5000, {iap_param(0, sent_group), iap_param(1, recv_group)});
    send_csm(0x5200);
    send_csm(0xae00, {iap_param(3), iap_param(4), iap_param(5)});
    send_csm(0x4157, {iap_param(0), iap_param(3), iap_param(4)});
    send_csm(0x4154, {iap_param(0), iap_param(1), iap_param(2), iap_param(3), iap_param(4), iap_param(10)});
    return;
  }
  if (!authenticated_) return;
  if (id == 0x5702) send_wifi();
  if (id == 0x4e0e) {
    transport_notified_ = true;
    send_wifi();
  }
  if (id == 0x4300) start_carplay();
}

}  // namespace carplay
