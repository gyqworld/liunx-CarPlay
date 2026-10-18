#pragma once

#include "carplay/protocol/iap_accessory.hpp"

#include <string>

namespace carplay {

/**
 * Loads identity.pk8 + certificate.p7b from a directory (same names as LinuxPlay).
 * Signing uses OpenSSL P-256 when CP_HAVE_OPENSSL is defined.
 */
class FileMfiSigner final : public IMfiSigner {
 public:
  explicit FileMfiSigner(std::string directory);

  bool available() const { return available_; }
  const std::string& error() const { return error_; }
  ByteBuffer certificate() override;
  ByteBuffer sign_challenge(const ByteBuffer& challenge) override;

  static bool import_files(const std::string& directory, const std::string& pk8_path,
                           const std::string& p7b_path, std::string& error);

 private:
  std::string directory_;
  ByteBuffer cert_;
  ByteBuffer key_der_;
  bool available_ = false;
  std::string error_;
};

}  // namespace carplay
