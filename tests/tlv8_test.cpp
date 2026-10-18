#include "carplay/protocol/tlv8.hpp"

#include <iostream>

int main() {
  using carplay::ByteBuffer;
  using carplay::Tlv8;
  using carplay::Tlv8Codec;

  ByteBuffer long_value(300, 0x5a);
  const std::vector<Tlv8> items = {{0x01, {0x01, 0x02}}, {0x03, long_value}};
  const auto encoded = Tlv8Codec::encode(items);
  const auto decoded = Tlv8Codec::decode(encoded);
  if (decoded.size() != 2) {
    std::cerr << "expected 2 items, got " << decoded.size() << "\n";
    return 1;
  }
  if (decoded[1].value != long_value) {
    std::cerr << "long value was not reassembled\n";
    return 1;
  }
  const auto found = Tlv8Codec::find(decoded, 0x01);
  if (!found || found->size() != 2) {
    std::cerr << "find failed\n";
    return 1;
  }
  return 0;
}
