#include "carplay/protocol/pair_verify.hpp"

#include "carplay/protocol/tlv8.hpp"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <string>

#include <cstdlib>

#if defined(CP_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/kdf.h>
#endif

namespace carplay {
namespace {

constexpr std::uint8_t kIdentifier = 0x01;
constexpr std::uint8_t kPublicKey = 0x03;
constexpr std::uint8_t kEncrypted = 0x05;
constexpr std::uint8_t kState = 0x06;
constexpr std::uint8_t kError = 0x07;
constexpr std::uint8_t kSignature = 0x0a;

std::string pairing_dir() {
  const char* home = std::getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.config/carplay-cpp/pairing";
}

ByteBuffer tlv_items(std::initializer_list<Tlv8> items) { return Tlv8Codec::encode(std::vector<Tlv8>(items)); }

#if defined(CP_HAVE_OPENSSL)

ByteBuffer hkdf_sha512(const ByteBuffer& ikm, const char* salt, const char* info) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  ByteBuffer out(32);
  std::size_t out_len = out.size();
  if (!ctx) return {};
  const bool ok = EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) > 0 &&
                  EVP_PKEY_CTX_set1_hkdf_salt(ctx, reinterpret_cast<const unsigned char*>(salt),
                                              static_cast<int>(std::char_traits<char>::length(salt))) > 0 &&
                  EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm.data(), static_cast<int>(ikm.size())) > 0 &&
                  EVP_PKEY_CTX_add1_hkdf_info(ctx, reinterpret_cast<const unsigned char*>(info),
                                              static_cast<int>(std::char_traits<char>::length(info))) > 0 &&
                  EVP_PKEY_derive(ctx, out.data(), &out_len) > 0 && out_len == 32;
  EVP_PKEY_CTX_free(ctx);
  return ok ? out : ByteBuffer{};
}

ByteBuffer nonce_label(const char* label) {
  ByteBuffer nonce(12, 0);
  const std::size_t n = std::char_traits<char>::length(label);
  if (n <= 8) std::copy(label, label + n, nonce.begin() + 4);
  return nonce;
}

ByteBuffer nonce_counter(std::uint64_t counter) {
  ByteBuffer nonce(12, 0);
  for (int i = 0; i < 8; ++i) nonce[static_cast<std::size_t>(4 + i)] = static_cast<std::uint8_t>((counter >> (8 * i)) & 0xff);
  return nonce;
}

ByteBuffer chacha_seal(const ByteBuffer& key, const ByteBuffer& nonce, const ByteBuffer& plain, const ByteBuffer& aad) {
  if (key.size() != 32 || nonce.size() != 12) return {};
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return {};
  int out_len = 0;
  int aad_len = 0;
  int final_len = 0;
  ByteBuffer cipher(plain.size());
  unsigned char tag[16];
  const bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
                  EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
                  (aad.empty() || EVP_EncryptUpdate(ctx, nullptr, &aad_len, aad.data(), static_cast<int>(aad.size())) == 1) &&
                  EVP_EncryptUpdate(ctx, plain.empty() ? nullptr : cipher.data(), &out_len, plain.empty() ? nullptr : plain.data(),
                                    static_cast<int>(plain.size())) == 1 &&
                  EVP_EncryptFinal_ex(ctx, cipher.data() + out_len, &final_len) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return {};
  cipher.resize(static_cast<std::size_t>(out_len + final_len));
  cipher.insert(cipher.end(), tag, tag + 16);
  return cipher;
}

ByteBuffer chacha_open(const ByteBuffer& key, const ByteBuffer& nonce, const ByteBuffer& data, const ByteBuffer& aad) {
  if (key.size() != 32 || nonce.size() != 12 || data.size() < 16) return {};
  const std::size_t cipher_len = data.size() - 16;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return {};
  int out_len = 0;
  int aad_len = 0;
  int final_len = 0;
  ByteBuffer plain(cipher_len);
  const bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
                  EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
                  (aad.empty() || EVP_DecryptUpdate(ctx, nullptr, &aad_len, aad.data(), static_cast<int>(aad.size())) == 1) &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                      const_cast<std::uint8_t*>(data.data() + cipher_len)) == 1 &&
                  EVP_DecryptUpdate(ctx, plain.empty() ? nullptr : plain.data(), &out_len, cipher_len ? data.data() : nullptr,
                                    static_cast<int>(cipher_len)) == 1 &&
                  EVP_DecryptFinal_ex(ctx, plain.data() + out_len, &final_len) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return {};
  plain.resize(static_cast<std::size_t>(out_len + final_len));
  return plain;
}

bool ed25519_sign(const ByteBuffer& priv, const ByteBuffer& data, ByteBuffer& sig) {
  if (priv.size() != 32) return false;
  EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv.data(), priv.size());
  if (!key) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  std::size_t len = 64;
  sig.assign(64, 0);
  const bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
                  EVP_DigestSign(ctx, sig.data(), &len, data.data(), data.size()) == 1 && len == 64;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

bool ed25519_verify(const ByteBuffer& pub, const ByteBuffer& data, const ByteBuffer& sig) {
  if (pub.size() != 32 || sig.size() != 64) return false;
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
  if (!key) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
                  EVP_DigestVerify(ctx, sig.data(), sig.size(), data.data(), data.size()) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

bool x25519_generate(ByteBuffer& priv, ByteBuffer& pub) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
  EVP_PKEY* key = nullptr;
  if (!ctx || EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &key) != 1 || !key) {
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    return false;
  }
  priv.assign(32, 0);
  pub.assign(32, 0);
  std::size_t len = 32;
  const bool ok = EVP_PKEY_get_raw_private_key(key, priv.data(), &len) == 1 && len == 32;
  len = 32;
  const bool pub_ok = ok && EVP_PKEY_get_raw_public_key(key, pub.data(), &len) == 1 && len == 32;
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(ctx);
  return pub_ok;
}

ByteBuffer x25519_shared(const ByteBuffer& priv, const ByteBuffer& peer) {
  if (priv.size() != 32 || peer.size() != 32) return {};
  EVP_PKEY* ours = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
  EVP_PKEY* theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer.data(), peer.size());
  EVP_PKEY_CTX* ctx = ours ? EVP_PKEY_CTX_new(ours, nullptr) : nullptr;
  ByteBuffer secret(32);
  std::size_t len = 32;
  const bool ok = ours && theirs && ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, theirs) > 0 &&
                  EVP_PKEY_derive(ctx, secret.data(), &len) > 0 && len == 32;
  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(theirs);
  EVP_PKEY_free(ours);
  return ok ? secret : ByteBuffer{};
}

struct Identity {
  ByteBuffer priv;
  ByteBuffer pub;
  std::string pairing_id;
};

bool load_identity(Identity& id) {
  std::ifstream in(pairing_dir() + "/identity");
  std::string priv_hex;
  std::string pub_hex;
  if (!in || !std::getline(in, priv_hex) || !std::getline(in, pub_hex) || !std::getline(in, id.pairing_id)) return false;
  id.priv = bytes_from_hex(priv_hex);
  id.pub = bytes_from_hex(pub_hex);
  return id.priv.size() == 32 && id.pub.size() == 32 && !id.pairing_id.empty();
}

ByteBuffer load_controller(const ByteBuffer& id) {
  std::ifstream in(pairing_dir() + "/controllers");
  const std::string want(id.begin(), id.end());
  std::string line;
  while (std::getline(in, line)) {
    const auto split = line.rfind(' ');
    if (split == std::string::npos) continue;
    if (line.substr(0, split) != want) continue;
    return bytes_from_hex(line.substr(split + 1));
  }
  return {};
}

ByteBuffer sha256(const ByteBuffer& a, const ByteBuffer& b) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  ByteBuffer out(32);
  unsigned len = 0;
  if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, a.data(), a.size()) != 1 || EVP_DigestUpdate(ctx, b.data(), b.size()) != 1 ||
      EVP_DigestFinal_ex(ctx, out.data(), &len) != 1 || len != 32) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  EVP_MD_CTX_free(ctx);
  return out;
}

ByteBuffer aes_ctr(const ByteBuffer& key, const ByteBuffer& iv, const ByteBuffer& data) {
  if (key.size() != 16 || iv.size() != 16) return {};
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  ByteBuffer out(data.size());
  int len = 0;
  int final_len = 0;
  const bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key.data(), iv.data()) == 1 &&
                  EVP_EncryptUpdate(ctx, out.data(), &len, data.data(), static_cast<int>(data.size())) == 1 &&
                  EVP_EncryptFinal_ex(ctx, out.data() + len, &final_len) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return {};
  out.resize(static_cast<std::size_t>(len + final_len));
  return out;
}

ByteBuffer sha1_16(const char* label, const ByteBuffer& shared) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  ByteBuffer full(20);
  unsigned len = 0;
  if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, label, std::char_traits<char>::length(label)) != 1 ||
      EVP_DigestUpdate(ctx, shared.data(), shared.size()) != 1 || EVP_DigestFinal_ex(ctx, full.data(), &len) != 1) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  EVP_MD_CTX_free(ctx);
  full.resize(16);
  return full;
}

#endif

}  // namespace

ControlCipher::ControlCipher(ByteBuffer read_key, ByteBuffer write_key)
    : read_key_(std::move(read_key)), write_key_(std::move(write_key)) {}

std::string ControlCipher::open(std::string& pending) {
#if !defined(CP_HAVE_OPENSSL)
  (void)pending;
  return {};
#else
  std::string plain;
  std::size_t off = 0;
  while (pending.size() >= off + 2) {
    const auto length = static_cast<std::uint16_t>(static_cast<unsigned char>(pending[off]) |
                                                   (static_cast<unsigned char>(pending[off + 1]) << 8));
    if (length > 0x4000) {
      pending.clear();
      return {};
    }
    const std::size_t frame_end = off + 2 + length + 16;
    if (pending.size() < frame_end) break;
    ByteBuffer aad{static_cast<std::uint8_t>(pending[off]), static_cast<std::uint8_t>(pending[off + 1])};
    ByteBuffer blob(pending.begin() + static_cast<std::ptrdiff_t>(off + 2),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
    const ByteBuffer opened = chacha_open(read_key_, nonce_counter(read_count_), blob, aad);
    if (opened.empty() && length != 0) {
      pending.clear();
      return {};
    }
    plain.append(reinterpret_cast<const char*>(opened.data()), opened.size());
    ++read_count_;
    off = frame_end;
  }
  pending.erase(0, off);
  return plain;
#endif
}

std::string ControlCipher::seal(const std::string& plain) {
#if !defined(CP_HAVE_OPENSSL)
  (void)plain;
  return {};
#else
  std::string out;
  std::size_t offset = 0;
  do {
    const std::size_t n = std::min<std::size_t>(0x4000, plain.size() - offset);
    ByteBuffer chunk(plain.begin() + static_cast<std::ptrdiff_t>(offset),
                     plain.begin() + static_cast<std::ptrdiff_t>(offset + n));
    ByteBuffer header{static_cast<std::uint8_t>(n & 0xff), static_cast<std::uint8_t>((n >> 8) & 0xff)};
    const ByteBuffer sealed = chacha_seal(write_key_, nonce_counter(write_count_), chunk, header);
    if (sealed.empty()) return {};
    out.append(reinterpret_cast<const char*>(header.data()), header.size());
    out.append(reinterpret_cast<const char*>(sealed.data()), sealed.size());
    ++write_count_;
    offset += n;
  } while (offset < plain.size());
  return out;
#endif
}

ByteBuffer PairVerify::error_tlv(std::uint8_t state) {
  return tlv_items({Tlv8{kState, ByteBuffer{state}}, Tlv8{kError, ByteBuffer{2}}});
}

ByteBuffer PairVerify::handle(const std::uint8_t* data, std::size_t len) {
  const auto items = Tlv8Codec::decode(data, len);
  const auto state_buf = Tlv8Codec::find(items, kState);
  const std::uint8_t state = state_buf && !state_buf->empty() ? (*state_buf)[0] : 0;
  if (state == 1) {
    const auto pub = Tlv8Codec::find(items, kPublicKey);
    if (!pub) return error_tlv(2);
    return m2(*pub);
  }
  if (state == 3) {
    const auto enc = Tlv8Codec::find(items, kEncrypted);
    if (!enc) return error_tlv(4);
    return m4(*enc);
  }
  return error_tlv(state == 0 ? 2 : static_cast<std::uint8_t>(state + 1));
}

ByteBuffer PairVerify::m2(const ByteBuffer& client_pub) {
#if !defined(CP_HAVE_OPENSSL)
  (void)client_pub;
  return error_tlv(2);
#else
  ByteBuffer priv;
  if (client_pub.size() != 32 || !x25519_generate(priv, our_pub_)) return error_tlv(2);
  client_pub_ = client_pub;
  shared_ = x25519_shared(priv, client_pub_);
  enc_key_ = hkdf_sha512(shared_, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
  if (shared_.size() != 32 || enc_key_.size() != 32) return error_tlv(2);
  Identity id;
  if (!load_identity(id)) return error_tlv(2);
  const ByteBuffer acc_id(id.pairing_id.begin(), id.pairing_id.end());
  ByteBuffer signed_data = our_pub_;
  signed_data.insert(signed_data.end(), acc_id.begin(), acc_id.end());
  signed_data.insert(signed_data.end(), client_pub_.begin(), client_pub_.end());
  ByteBuffer sig;
  if (!ed25519_sign(id.priv, signed_data, sig)) return error_tlv(2);
  const ByteBuffer sub = tlv_items({Tlv8{kIdentifier, acc_id}, Tlv8{kSignature, sig}});
  const ByteBuffer sealed = chacha_seal(enc_key_, nonce_label("PV-Msg02"), sub, {});
  if (sealed.empty()) return error_tlv(2);
  return tlv_items({Tlv8{kState, ByteBuffer{2}}, Tlv8{kPublicKey, our_pub_}, Tlv8{kEncrypted, sealed}});
#endif
}

ByteBuffer PairVerify::m4(const ByteBuffer& encrypted) {
#if !defined(CP_HAVE_OPENSSL)
  (void)encrypted;
  return error_tlv(4);
#else
  if (enc_key_.empty() || shared_.empty() || our_pub_.empty() || client_pub_.empty()) return error_tlv(4);
  const ByteBuffer opened = chacha_open(enc_key_, nonce_label("PV-Msg03"), encrypted, {});
  if (opened.empty()) return error_tlv(4);
  const auto sub = Tlv8Codec::decode(opened);
  const auto ctrl_id = Tlv8Codec::find(sub, kIdentifier);
  const auto ctrl_sig = Tlv8Codec::find(sub, kSignature);
  if (!ctrl_id || !ctrl_sig) return error_tlv(4);
  const ByteBuffer ctrl_pub = load_controller(*ctrl_id);
  if (ctrl_pub.size() != 32) return error_tlv(4);
  ByteBuffer signed_data = client_pub_;
  signed_data.insert(signed_data.end(), ctrl_id->begin(), ctrl_id->end());
  signed_data.insert(signed_data.end(), our_pub_.begin(), our_pub_.end());
  if (!ed25519_verify(ctrl_pub, signed_data, *ctrl_sig)) return error_tlv(4);
  read_key_ = hkdf_sha512(shared_, "Control-Salt", "Control-Write-Encryption-Key");
  write_key_ = hkdf_sha512(shared_, "Control-Salt", "Control-Read-Encryption-Key");
  if (read_key_.size() != 32 || write_key_.size() != 32) return error_tlv(4);
  verified_ = true;
  return tlv_items({Tlv8{kState, ByteBuffer{4}}});
#endif
}

ByteBuffer auth_setup_response(const std::uint8_t* data, std::size_t len, IMfiSigner& signer) {
#if !defined(CP_HAVE_OPENSSL)
  (void)data;
  (void)len;
  (void)signer;
  return {};
#else
  if (data == nullptr || len != 33 || data[0] != 1) return {};
  const ByteBuffer peer(data + 1, data + 33);
  ByteBuffer priv;
  ByteBuffer pub;
  if (!x25519_generate(priv, pub)) return {};
  const ByteBuffer shared = x25519_shared(priv, peer);
  if (shared.size() != 32) return {};
  const ByteBuffer key = sha1_16("AES-KEY", shared);
  const ByteBuffer iv = sha1_16("AES-IV", shared);
  ByteBuffer material = pub;
  material.insert(material.end(), peer.begin(), peer.end());
  const ByteBuffer digest = sha256(pub, peer);
  const ByteBuffer signature = signer.sign_challenge(digest);
  if (signature.empty() || key.empty() || iv.empty()) return {};
  const ByteBuffer enc = aes_ctr(key, iv, signature);
  if (enc.empty()) return {};
  const ByteBuffer cert = signer.certificate();
  ByteBuffer out = pub;
  const auto cert_len = static_cast<std::uint32_t>(cert.size());
  const auto sig_len = static_cast<std::uint32_t>(enc.size());
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>((cert_len >> shift) & 0xff));
  out.insert(out.end(), cert.begin(), cert.end());
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>((sig_len >> shift) & 0xff));
  out.insert(out.end(), enc.begin(), enc.end());
  (void)material;
  return out;
#endif
}

}  // namespace carplay
