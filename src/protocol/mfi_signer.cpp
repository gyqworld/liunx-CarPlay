#include "carplay/protocol/mfi_signer.hpp"

#include <fstream>
#include <sys/stat.h>

#if defined(CP_HAVE_OPENSSL)
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

namespace carplay {
namespace {

ByteBuffer read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return ByteBuffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, const ByteBuffer& data) {
  const std::string tmp = path + ".new";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  out.close();
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

FileMfiSigner::FileMfiSigner(std::string directory) : directory_(std::move(directory)) {
  key_der_ = read_file(directory_ + "/identity.pk8");
  cert_ = read_file(directory_ + "/certificate.p7b");
  if (key_der_.empty() || cert_.empty()) {
    error_ = "请先导入认证文件（identity.pk8 与 certificate.p7b）";
    return;
  }
  available_ = true;
}

ByteBuffer FileMfiSigner::certificate() { return available_ ? cert_ : ByteBuffer{}; }

ByteBuffer FileMfiSigner::sign_challenge(const ByteBuffer& challenge) {
  if (!available_ || challenge.size() != 32) return {};
#if defined(CP_HAVE_OPENSSL)
  const unsigned char* ptr = key_der_.data();
  EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &ptr, static_cast<long>(key_der_.size()));
  if (!key) return {};
  EC_KEY* ec = EVP_PKEY_get1_EC_KEY(key);
  EVP_PKEY_free(key);
  if (!ec) return {};
  ECDSA_SIG* sig = ECDSA_do_sign(challenge.data(), static_cast<int>(challenge.size()), ec);
  EC_KEY_free(ec);
  if (!sig) return {};
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(sig, &r, &s);
  ByteBuffer out(64);
  if (BN_bn2binpad(r, out.data(), 32) != 32 || BN_bn2binpad(s, out.data() + 32, 32) != 32) {
    ECDSA_SIG_free(sig);
    return {};
  }
  ECDSA_SIG_free(sig);
  return out;
#else
  (void)challenge;
  error_ = "未编译 OpenSSL，无法完成 MFi 签名";
  return {};
#endif
}

bool FileMfiSigner::import_files(const std::string& directory, const std::string& pk8_path,
                                 const std::string& p7b_path, std::string& error) {
  const ByteBuffer key = read_file(pk8_path);
  const ByteBuffer cert = read_file(p7b_path);
  if (key.empty() || cert.empty()) {
    error = "无法读取认证文件";
    return false;
  }
  if (key.size() > 16384 || cert.size() > 1024 * 1024) {
    error = "认证文件大小不合理";
    return false;
  }
  ::mkdir(directory.c_str(), 0700);
  if (!write_file(directory + "/identity.pk8", key) || !write_file(directory + "/certificate.p7b", cert)) {
    error = "无法写入认证目录";
    return false;
  }
  return true;
}

}  // namespace carplay
