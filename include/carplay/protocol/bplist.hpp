#pragma once

#include "carplay/protocol/buffer_util.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace carplay {

struct Plist;
using PlistDict = std::map<std::string, Plist>;
using PlistArray = std::vector<Plist>;

struct Plist {
  using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, ByteBuffer, PlistArray, PlistDict>;
  Value value;

  static Plist boolean(bool v) { return Plist{v}; }
  static Plist integer(std::int64_t v) { return Plist{v}; }
  static Plist real(double v) { return Plist{v}; }
  static Plist string(std::string v) { return Plist{std::move(v)}; }
  static Plist data(ByteBuffer v) { return Plist{std::move(v)}; }
  static Plist array(PlistArray v) { return Plist{std::move(v)}; }
  static Plist dict(PlistDict v) { return Plist{std::move(v)}; }

  bool is_dict() const { return std::holds_alternative<PlistDict>(value); }
  bool is_array() const { return std::holds_alternative<PlistArray>(value); }
  const PlistDict* as_dict() const { return std::get_if<PlistDict>(&value); }
  const PlistArray* as_array() const { return std::get_if<PlistArray>(&value); }
  std::int64_t as_int(std::int64_t fallback = 0) const;
  bool as_bool(bool fallback = false) const;
  std::string as_string() const;
};

ByteBuffer encode_bplist(const Plist& root);
bool decode_bplist(const std::uint8_t* data, std::size_t len, Plist& out);

}  // namespace carplay
