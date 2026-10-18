#include "carplay/protocol/buffer_util.hpp"

#include <cctype>
#include <stdexcept>

namespace carplay {
namespace {

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  throw std::invalid_argument("invalid hex");
}

}  // namespace

ByteBuffer bytes_from_hex(const std::string& hex) {
  std::string cleaned;
  cleaned.reserve(hex.size());
  for (char c : hex) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == ':' || c == '-') continue;
    cleaned.push_back(c);
  }
  if (cleaned.size() % 2 != 0) throw std::invalid_argument("odd hex length");
  ByteBuffer out(cleaned.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint8_t>((hex_nibble(cleaned[i * 2]) << 4) |
                                       hex_nibble(cleaned[i * 2 + 1]));
  }
  return out;
}

std::string hex_from_bytes(const std::uint8_t* data, std::size_t len) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out[i * 2] = kDigits[(data[i] >> 4) & 0xf];
    out[i * 2 + 1] = kDigits[data[i] & 0xf];
  }
  return out;
}

std::uint16_t read_be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void write_be16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>((v >> 8) & 0xff);
  p[1] = static_cast<std::uint8_t>(v & 0xff);
}

void write_be32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>((v >> 24) & 0xff);
  p[1] = static_cast<std::uint8_t>((v >> 16) & 0xff);
  p[2] = static_cast<std::uint8_t>((v >> 8) & 0xff);
  p[3] = static_cast<std::uint8_t>(v & 0xff);
}

}  // namespace carplay
