#include "carplay/protocol/tlv8.hpp"

namespace carplay {

ByteBuffer Tlv8Codec::encode(const std::vector<Tlv8>& items) {
  ByteBuffer out;
  for (const auto& item : items) {
    if (item.value.empty()) {
      out.push_back(item.type);
      out.push_back(0);
      continue;
    }
    std::size_t remaining = item.value.size();
    std::size_t offset = 0;
    while (remaining > 0) {
      const std::size_t chunk = remaining > 255 ? 255 : remaining;
      out.push_back(item.type);
      out.push_back(static_cast<std::uint8_t>(chunk));
      out.insert(out.end(),
                 item.value.begin() + static_cast<std::ptrdiff_t>(offset),
                 item.value.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
      offset += chunk;
      remaining -= chunk;
    }
  }
  return out;
}

std::vector<Tlv8> Tlv8Codec::decode(const std::uint8_t* data, std::size_t len) {
  std::vector<Tlv8> items;
  std::size_t i = 0;
  while (i + 2 <= len) {
    Tlv8 item;
    item.type = data[i++];
    const std::uint8_t size = data[i++];
    if (i + size > len) break;
    item.value.assign(data + i, data + i + size);
    i += size;
    if (!items.empty() && items.back().type == item.type) {
      items.back().value.insert(items.back().value.end(), item.value.begin(), item.value.end());
    } else {
      items.push_back(std::move(item));
    }
  }
  return items;
}

std::optional<ByteBuffer> Tlv8Codec::find(const std::vector<Tlv8>& items, std::uint8_t type) {
  for (const auto& item : items) {
    if (item.type == type) return item.value;
  }
  return std::nullopt;
}

}  // namespace carplay
