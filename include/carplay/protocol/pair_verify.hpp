#pragma once

#include "carplay/protocol/buffer_util.hpp"
#include "carplay/protocol/iap_accessory.hpp"

#include <cstdint>
#include <string>

namespace carplay {

/** ChaCha20-Poly1305 frames used on the control connection after pair-verify. */
class ControlCipher {
 public:
  ControlCipher(ByteBuffer read_key, ByteBuffer write_key);

  /** Decrypt every complete frame in `pending`. Leaves a partial frame in place. */
  std::string open(std::string& pending);
  std::string seal(const std::string& plain);

 private:
  ByteBuffer read_key_;
  ByteBuffer write_key_;
  std::uint64_t read_count_ = 0;
  std::uint64_t write_count_ = 0;
};

/**
 * CarPlay pair-verify responder. On success the control keys match LinuxPlay:
 * the accessory reads with the controller write key and writes with its read key.
 */
class PairVerify {
 public:
  ByteBuffer handle(const std::uint8_t* data, std::size_t len);
  bool verified() const { return verified_; }
  const ByteBuffer& shared_secret() const { return shared_; }
  const ByteBuffer& read_key() const { return read_key_; }
  const ByteBuffer& write_key() const { return write_key_; }

 private:
  ByteBuffer m2(const ByteBuffer& client_pub);
  ByteBuffer m4(const ByteBuffer& encrypted);
  static ByteBuffer error_tlv(std::uint8_t state);

  ByteBuffer our_pub_;
  ByteBuffer client_pub_;
  ByteBuffer shared_;
  ByteBuffer enc_key_;
  ByteBuffer read_key_;
  ByteBuffer write_key_;
  bool verified_ = false;
};

/** MFiSAP /auth-setup body, or empty when the request cannot be signed. */
ByteBuffer auth_setup_response(const std::uint8_t* data, std::size_t len, IMfiSigner& signer);

}  // namespace carplay
