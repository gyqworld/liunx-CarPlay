#pragma once

#include "carplay/protocol/buffer_util.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace carplay {

/** Hap-style TLV8 used by AirPlay / pairing payloads. */
struct Tlv8 {
  std::uint8_t type = 0;
  ByteBuffer value;
};

class Tlv8Codec {
 public:
  static ByteBuffer encode(const std::vector<Tlv8>& items);
  static std::vector<Tlv8> decode(const std::uint8_t* data, std::size_t len);
  static std::vector<Tlv8> decode(const ByteBuffer& buf) {
    return decode(buf.data(), buf.size());
  }

  static std::optional<ByteBuffer> find(const std::vector<Tlv8>& items, std::uint8_t type);
};

}  // namespace carplay
