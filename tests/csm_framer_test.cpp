#include "carplay/protocol/csm_framer.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  using carplay::ByteBuffer;
  using carplay::CsmFramer;

  CsmFramer::Packet packet;
  packet.control = 0x40;
  packet.seq = 7;
  packet.ack = 3;
  packet.session_id = 2;
  packet.payload = {0x10, 0x20, 0x30, 0x40};

  auto encoded = CsmFramer::encode(packet);
  // Prefix noise that the framer must skip.
  ByteBuffer stream = {0x00, 0x11};
  stream.insert(stream.end(), encoded.begin(), encoded.end());
  stream.push_back(0xff);

  auto decoded = CsmFramer::try_decode(stream);
  if (!decoded) {
    std::cerr << "decode failed\n";
    return 1;
  }
  if (decoded->seq != 7 || decoded->payload != packet.payload) {
    std::cerr << "payload mismatch\n";
    return 1;
  }
  if (stream.size() != 1 || stream[0] != 0xff) {
    std::cerr << "leftover bytes wrong\n";
    return 1;
  }
  unsigned header_sum = 0;
  for (int i = 0; i < 9; ++i) header_sum += encoded[static_cast<std::size_t>(i)];
  if ((header_sum & 0xff) != 0 || encoded.size() != packet.payload.size() + 10) {
    std::cerr << "header checksum or length mismatch\n";
    return 1;
  }
  const carplay::ByteBuffer detect(CsmFramer::kDetect, CsmFramer::kDetect + 6);
  const carplay::ByteBuffer expect = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
  if (detect != expect) {
    std::cerr << "detect marker mismatch\n";
    return 1;
  }
  return 0;
}
