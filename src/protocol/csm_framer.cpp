#include "carplay/protocol/csm_framer.hpp"

#include <algorithm>

namespace carplay {

std::uint8_t CsmFramer::checksum(const std::uint8_t* data, std::size_t len) {
  unsigned sum = 0;
  for (std::size_t i = 0; i < len; ++i) sum += data[i];
  return static_cast<std::uint8_t>((0x100 - (sum & 0xff)) & 0xff);
}

ByteBuffer CsmFramer::encode(const Packet& packet) {
  const std::size_t payload_len = packet.payload.size();
  const bool has_payload = payload_len != 0;
  const std::uint16_t length = static_cast<std::uint16_t>(has_payload ? payload_len + 10 : 9);
  ByteBuffer out(length);
  write_be16(out.data(), kSync);
  write_be16(out.data() + 2, length);
  out[4] = packet.control;
  out[5] = packet.seq;
  out[6] = packet.ack;
  out[7] = packet.session_id;
  out[8] = checksum(out.data(), 8);
  if (has_payload) {
    std::copy(packet.payload.begin(), packet.payload.end(), out.begin() + 9);
    out[length - 1] = checksum(out.data() + 9, payload_len);
  }
  return out;
}

std::optional<CsmFramer::Packet> CsmFramer::try_decode(ByteBuffer& stream) {
  while (stream.size() >= 2) {
    if (read_be16(stream.data()) != kSync) {
      stream.erase(stream.begin());
      continue;
    }
    break;
  }
  if (stream.size() < 9) return std::nullopt;
  if (read_be16(stream.data()) != kSync) return std::nullopt;

  const std::uint16_t length = read_be16(stream.data() + 2);
  if (length < 9) {
    stream.erase(stream.begin());
    return std::nullopt;
  }
  if (stream.size() < length) return std::nullopt;

  if (stream[8] != checksum(stream.data(), 8)) {
    stream.erase(stream.begin());
    return std::nullopt;
  }
  if (length > 9 && stream[length - 1] != checksum(stream.data() + 9, static_cast<std::size_t>(length - 10))) {
    stream.erase(stream.begin());
    return std::nullopt;
  }

  Packet packet;
  packet.control = stream[4];
  packet.seq = stream[5];
  packet.ack = stream[6];
  packet.session_id = stream[7];
  if (length > 9) packet.payload.assign(stream.begin() + 9, stream.begin() + (length - 1));
  stream.erase(stream.begin(), stream.begin() + length);
  return packet;
}

}  // namespace carplay
