#pragma once

#include "carplay/protocol/buffer_util.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace carplay {

/**
 * iAP2 link framing, matching LinuxPlay src/engine/iap.ts packet().
 * Layout (big-endian):
 *   sync(2)=0xFF5A | length(2) | control | seq | ack | session | header checksum
 *   | payload | payload checksum
 * Length is 9 when there is no payload, otherwise payload size + 10.
 * The 6-byte detect marker is separate from these packets.
 */
class CsmFramer {
 public:
  static constexpr std::uint16_t kSync = 0xFF5A;
  static constexpr std::uint8_t kDetect[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};

  struct Packet {
    std::uint8_t control = 0;
    std::uint8_t seq = 0;
    std::uint8_t ack = 0;
    std::uint8_t session_id = 0;
    ByteBuffer payload;
  };

  /** Build a complete CSM frame including checksum. */
  static ByteBuffer encode(const Packet& packet);

  /**
   * Try to parse one complete frame from the front of `stream`.
   * On success, removes consumed bytes from `stream` and returns the packet.
   */
  static std::optional<Packet> try_decode(ByteBuffer& stream);

  static std::uint8_t checksum(const std::uint8_t* data, std::size_t len);
};

}  // namespace carplay
