#include "carplay/protocol/bplist.hpp"

#include <cstring>
#include <functional>
#include <limits>

namespace carplay {
namespace {

void write_be(ByteBuffer& out, std::uint64_t value, int size) {
  for (int i = size - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
}

std::uint64_t read_be(const std::uint8_t* data, int size) {
  std::uint64_t v = 0;
  for (int i = 0; i < size; ++i) v = (v << 8) | data[i];
  return v;
}

ByteBuffer marker(int type, std::size_t count) {
  if (count < 15) return ByteBuffer{static_cast<std::uint8_t>((type << 4) | count)};
  ByteBuffer out{static_cast<std::uint8_t>((type << 4) | 0x0f)};
  int bytes = 1;
  if (count > 0xff) bytes = 2;
  if (count > 0xffff) bytes = 4;
  if (count > 0xffffffffULL) bytes = 8;
  const int log = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
  out.push_back(static_cast<std::uint8_t>(0x10 | log));
  write_be(out, count, bytes);
  return out;
}

ByteBuffer encode_int(std::int64_t n) {
  std::uint64_t v = static_cast<std::uint64_t>(n);
  int bytes = 1;
  if (v > 0xff) bytes = 2;
  if (v > 0xffff) bytes = 4;
  if (v > 0xffffffffULL) bytes = 8;
  const int log = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
  ByteBuffer out{static_cast<std::uint8_t>(0x10 | log)};
  write_be(out, v, bytes);
  return out;
}

bool is_ascii(const std::string& text) {
  for (unsigned char c : text)
    if (c > 0x7f) return false;
  return true;
}

/** UTF-8 → UTF-16 code units. Binary plist type 0x6 counts units, not UTF-8 bytes. */
std::u16string utf8_to_utf16(const std::string& text) {
  std::u16string out;
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = p + text.size();
  while (p < end) {
    std::uint32_t cp = 0xFFFD;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
      cp = (static_cast<std::uint32_t>(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
      cp = (static_cast<std::uint32_t>(p[0] & 0x0F) << 12) | (static_cast<std::uint32_t>(p[1] & 0x3F) << 6) |
           (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
      cp = (static_cast<std::uint32_t>(p[0] & 0x07) << 18) | (static_cast<std::uint32_t>(p[1] & 0x3F) << 12) |
           (static_cast<std::uint32_t>(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      p += 4;
    } else {
      ++p;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

struct EncNode {
  ByteBuffer head;
  ByteBuffer body;
  std::vector<int> refs;
};

int add_node(std::vector<EncNode>& nodes, const Plist& value) {
  const int idx = static_cast<int>(nodes.size());
  nodes.push_back({});
  if (std::holds_alternative<bool>(value.value)) {
    nodes[idx].body = ByteBuffer{static_cast<std::uint8_t>(std::get<bool>(value.value) ? 0x09 : 0x08)};
  } else if (std::holds_alternative<std::int64_t>(value.value)) {
    nodes[idx].body = encode_int(std::get<std::int64_t>(value.value));
  } else if (std::holds_alternative<double>(value.value)) {
    ByteBuffer body{0x23};
    const double d = std::get<double>(value.value);
    std::uint8_t raw[8];
    std::memcpy(raw, &d, 8);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (int i = 7; i >= 0; --i) body.push_back(raw[i]);
#else
    body.insert(body.end(), raw, raw + 8);
#endif
    nodes[idx].body = std::move(body);
  } else if (std::holds_alternative<std::string>(value.value)) {
    const auto& text = std::get<std::string>(value.value);
    if (is_ascii(text)) {
      nodes[idx].body = marker(0x5, text.size());
      nodes[idx].body.insert(nodes[idx].body.end(), text.begin(), text.end());
    } else {
      const std::u16string units = utf8_to_utf16(text);
      nodes[idx].body = marker(0x6, units.size());
      for (char16_t unit : units) {
        nodes[idx].body.push_back(static_cast<std::uint8_t>(unit >> 8));
        nodes[idx].body.push_back(static_cast<std::uint8_t>(unit & 0xFF));
      }
    }
  } else if (std::holds_alternative<ByteBuffer>(value.value)) {
    const auto& data = std::get<ByteBuffer>(value.value);
    nodes[idx].body = marker(0x4, data.size());
    nodes[idx].body.insert(nodes[idx].body.end(), data.begin(), data.end());
  } else if (std::holds_alternative<PlistArray>(value.value)) {
    const PlistArray arr = std::get<PlistArray>(value.value);
    std::vector<int> refs;
    refs.reserve(arr.size());
    for (const auto& item : arr) refs.push_back(add_node(nodes, item));
    nodes[idx].refs = std::move(refs);
    nodes[idx].head = marker(0xa, arr.size());
  } else if (std::holds_alternative<PlistDict>(value.value)) {
    const PlistDict dict = std::get<PlistDict>(value.value);
    std::vector<int> refs;
    refs.reserve(dict.size() * 2);
    std::vector<std::string> keys;
    for (const auto& kv : dict) keys.push_back(kv.first);
    for (const auto& key : keys) refs.push_back(add_node(nodes, Plist::string(key)));
    for (const auto& key : keys) refs.push_back(add_node(nodes, dict.at(key)));
    nodes[idx].refs = std::move(refs);
    nodes[idx].head = marker(0xd, dict.size());
  } else {
    nodes[idx].body = ByteBuffer{0x00};
  }
  return idx;
}

}  // namespace

std::int64_t Plist::as_int(std::int64_t fallback) const {
  if (const auto* v = std::get_if<std::int64_t>(&value)) return *v;
  if (const auto* v = std::get_if<double>(&value)) return static_cast<std::int64_t>(*v);
  return fallback;
}

bool Plist::as_bool(bool fallback) const {
  if (const auto* v = std::get_if<bool>(&value)) return *v;
  return fallback;
}

std::string Plist::as_string() const {
  if (const auto* v = std::get_if<std::string>(&value)) return *v;
  if (const auto* v = std::get_if<std::int64_t>(&value)) return std::to_string(*v);
  if (const auto* v = std::get_if<double>(&value)) return std::to_string(static_cast<std::int64_t>(*v));
  if (const auto* v = std::get_if<bool>(&value)) return *v ? "true" : "false";
  return {};
}

ByteBuffer encode_bplist(const Plist& root) {
  std::vector<EncNode> nodes;
  const int top = add_node(nodes, root);
  const int num = static_cast<int>(nodes.size());
  const int ref_size = num > 0xffff ? 4 : num > 0xff ? 2 : 1;

  std::vector<ByteBuffer> serialized;
  serialized.reserve(nodes.size());
  for (const auto& node : nodes) {
    if (!node.head.empty()) {
      ByteBuffer body = node.head;
      for (int ref : node.refs) write_be(body, static_cast<std::uint64_t>(ref), ref_size);
      serialized.push_back(std::move(body));
    } else {
      serialized.push_back(node.body);
    }
  }

  ByteBuffer out;
  out.insert(out.end(), {'b', 'p', 'l', 'i', 's', 't', '0', '0'});
  std::vector<std::size_t> offsets;
  for (const auto& part : serialized) {
    offsets.push_back(out.size());
    out.insert(out.end(), part.begin(), part.end());
  }
  const std::size_t offset_table = out.size();
  const int offset_size = out.size() > 0xffff ? 4 : out.size() > 0xff ? 2 : 1;
  for (std::size_t off : offsets) write_be(out, off, offset_size);
  ByteBuffer trailer(32, 0);
  trailer[6] = static_cast<std::uint8_t>(offset_size);
  trailer[7] = static_cast<std::uint8_t>(ref_size);
  for (int i = 0; i < 8; ++i)
    trailer[8 + i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(num) >> (56 - 8 * i)) & 0xff);
  for (int i = 0; i < 8; ++i)
    trailer[16 + i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(top) >> (56 - 8 * i)) & 0xff);
  for (int i = 0; i < 8; ++i)
    trailer[24 + i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(offset_table) >> (56 - 8 * i)) & 0xff);
  out.insert(out.end(), trailer.begin(), trailer.end());
  return out;
}

bool decode_bplist(const std::uint8_t* data, std::size_t len, Plist& out) {
  if (data == nullptr || len < 40 || std::memcmp(data, "bplist00", 8) != 0) return false;
  const std::uint8_t* trailer = data + len - 32;
  const int offset_size = trailer[6];
  const int ref_size = trailer[7];
  const auto num_objects = static_cast<std::size_t>(read_be(trailer + 8, 8));
  const auto top = static_cast<std::size_t>(read_be(trailer + 16, 8));
  const auto offset_table = static_cast<std::size_t>(read_be(trailer + 24, 8));
  if (offset_size < 1 || offset_size > 8 || ref_size < 1 || ref_size > 8) return false;
  if (top >= num_objects) return false;
  if (offset_table + num_objects * static_cast<std::size_t>(offset_size) > len - 32) return false;

  std::vector<std::size_t> offsets(num_objects);
  for (std::size_t i = 0; i < num_objects; ++i)
    offsets[i] = static_cast<std::size_t>(read_be(data + offset_table + i * offset_size, offset_size));

  std::function<bool(std::size_t, Plist&)> read = [&](std::size_t index, Plist& value) -> bool {
    if (index >= offsets.size()) return false;
    std::size_t p = offsets[index];
    if (p >= len) return false;
    const std::uint8_t mark = data[p++];
    const int type = mark >> 4;
    int nib = mark & 0x0f;
    auto read_count = [&]() -> std::size_t {
      if (nib != 0x0f) return static_cast<std::size_t>(nib);
      if (p >= len) return 0;
      const std::uint8_t sz = data[p++];
      const int int_bytes = 1 << (sz & 0x0f);
      if (p + static_cast<std::size_t>(int_bytes) > len) return 0;
      const auto c = static_cast<std::size_t>(read_be(data + p, int_bytes));
      p += static_cast<std::size_t>(int_bytes);
      return c;
    };
    if (type == 0x0) {
      if (nib == 0x08) {
        value = Plist::boolean(false);
        return true;
      }
      if (nib == 0x09) {
        value = Plist::boolean(true);
        return true;
      }
      return false;
    }
    if (type == 0x1) {
      const int nbytes = 1 << nib;
      if (p + static_cast<std::size_t>(nbytes) > len) return false;
      const std::uint64_t raw = read_be(data + p, nbytes);
      // CarPlay stream/session IDs often exceed signed int64; keep the decimal form.
      if (nbytes == 8 && raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        value = Plist::string(std::to_string(raw));
      } else {
        value = Plist::integer(static_cast<std::int64_t>(raw));
      }
      return true;
    }
    if (type == 0x2) {
      if ((1 << nib) != 8 || p + 8 > len) return false;
      std::uint8_t raw[8];
      for (int i = 0; i < 8; ++i) raw[i] = data[p + i];
      double d = 0;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      std::uint8_t swapped[8];
      for (int i = 0; i < 8; ++i) swapped[i] = raw[7 - i];
      std::memcpy(&d, swapped, 8);
#else
      std::memcpy(&d, raw, 8);
#endif
      value = Plist::real(d);
      return true;
    }
    if (type == 0x4) {
      const auto count = read_count();
      if (p + count > len) return false;
      value = Plist::data(ByteBuffer(data + p, data + p + count));
      return true;
    }
    if (type == 0x5) {
      const auto count = read_count();
      if (p + count > len) return false;
      value = Plist::string(std::string(reinterpret_cast<const char*>(data + p), count));
      return true;
    }
    if (type == 0x6) {
      const auto count = read_count();
      if (p + count * 2 > len) return false;
      std::string text;
      text.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t ch = (static_cast<unsigned>(data[p]) << 8) | data[p + 1];
        p += 2;
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < count) {
          const std::uint32_t low = (static_cast<unsigned>(data[p]) << 8) | data[p + 1];
          p += 2;
          ++i;
          if (low >= 0xDC00 && low <= 0xDFFF) ch = 0x10000 + ((ch - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(text, ch);
      }
      value = Plist::string(std::move(text));
      return true;
    }
    if (type == 0xa) {
      const auto count = read_count();
      PlistArray arr;
      arr.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        if (p + static_cast<std::size_t>(ref_size) > len) return false;
        const auto ref = static_cast<std::size_t>(read_be(data + p, ref_size));
        p += static_cast<std::size_t>(ref_size);
        Plist item;
        if (!read(ref, item)) return false;
        arr.push_back(std::move(item));
      }
      value = Plist::array(std::move(arr));
      return true;
    }
    if (type == 0xd) {
      const auto count = read_count();
      std::vector<std::size_t> key_refs(count);
      std::vector<std::size_t> val_refs(count);
      for (std::size_t i = 0; i < count; ++i) {
        if (p + static_cast<std::size_t>(ref_size) > len) return false;
        key_refs[i] = static_cast<std::size_t>(read_be(data + p, ref_size));
        p += static_cast<std::size_t>(ref_size);
      }
      for (std::size_t i = 0; i < count; ++i) {
        if (p + static_cast<std::size_t>(ref_size) > len) return false;
        val_refs[i] = static_cast<std::size_t>(read_be(data + p, ref_size));
        p += static_cast<std::size_t>(ref_size);
      }
      PlistDict dict;
      for (std::size_t i = 0; i < count; ++i) {
        Plist key;
        Plist val;
        if (!read(key_refs[i], key) || !read(val_refs[i], val)) return false;
        dict[key.as_string()] = std::move(val);
      }
      value = Plist::dict(std::move(dict));
      return true;
    }
    return false;
  };

  return read(top, out);
}

}  // namespace carplay
