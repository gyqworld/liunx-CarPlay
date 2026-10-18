#pragma once

#include "carplay/protocol/buffer_util.hpp"

#include <string>

namespace carplay {

/** Lowercase hex of the accessory Ed25519 public key (`pk` in the CarPlay start message). */
std::string pairing_public_hex();

/** Stable accessory deviceID derived from the Ed25519 public key (LinuxPlay accessoryDeviceId). */
std::string pairing_device_id();

/**
 * CarPlay pair-setup responder: SRP-6a 3072 / SHA-512, PIN 3939, then Ed25519.
 * One instance per control connection. Matches LinuxPlay pairSetup.ts.
 */
class PairSetup {
 public:
  ByteBuffer handle(const std::uint8_t* data, std::size_t len);
  bool complete() const { return complete_; }

 private:
  ByteBuffer m2();
  ByteBuffer m4(const ByteBuffer& client_pub, const ByteBuffer& client_proof);
  ByteBuffer m6(const ByteBuffer& encrypted);
  static ByteBuffer error_tlv(std::uint8_t state);

  ByteBuffer salt_;
  ByteBuffer server_pub_;
  ByteBuffer secret_;
  ByteBuffer verifier_;
  ByteBuffer session_key_;
  bool have_session_ = false;
  bool complete_ = false;
};

}  // namespace carplay
