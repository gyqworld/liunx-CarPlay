#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carplay {

using ByteBuffer = std::vector<std::uint8_t>;

ByteBuffer bytes_from_hex(const std::string& hex);
std::string hex_from_bytes(const std::uint8_t* data, std::size_t len);
inline std::string hex_from_bytes(const ByteBuffer& buf) {
  return hex_from_bytes(buf.data(), buf.size());
}

std::uint16_t read_be16(const std::uint8_t* p);
std::uint32_t read_be32(const std::uint8_t* p);
void write_be16(std::uint8_t* p, std::uint16_t v);
void write_be32(std::uint8_t* p, std::uint32_t v);

}  // namespace carplay
