#include "carplay/session/session_services.hpp"

#include "carplay/protocol/bplist.hpp"

#include <cstdlib>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#if defined(CP_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/kdf.h>
#endif

namespace carplay {
namespace {

constexpr int kPtRequest = 210;
constexpr int kPtResponse = 211;
constexpr std::uint64_t kNtpEpoch = 2208988800ULL;

std::uint64_t wall_ntp() {
  using namespace std::chrono;
  const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  const auto sec = static_cast<std::uint64_t>(ms / 1000) + kNtpEpoch;
  const auto frac = static_cast<std::uint64_t>(((ms % 1000) << 32) / 1000);
  return (sec << 32) | frac;
}

void write_ntp(std::uint8_t* buf, std::uint64_t ntp) {
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<std::uint8_t>((ntp >> (56 - 8 * i)) & 0xff);
}

std::uint64_t read_ntp(const std::uint8_t* buf) {
  std::uint64_t ntp = 0;
  for (int i = 0; i < 8; ++i) ntp = (ntp << 8) | buf[i];
  return ntp;
}

std::string normalize_host(std::string host) {
  if (host.rfind("::ffff:", 0) == 0) host = host.substr(7);
  const auto pct = host.find('%');
  if (pct != std::string::npos) host = host.substr(0, pct);
  return host;
}

int bind_tcp6(int& fd, std::string& error) {
  fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    error = "socket failed";
    return -1;
  }
  int one = 1;
  int off = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 4) != 0) {
    error = "bind/listen failed";
    ::close(fd);
    fd = -1;
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    error = "getsockname failed";
    ::close(fd);
    fd = -1;
    return -1;
  }
  return ntohs(addr.sin6_port);
}

int bind_udp6(int& fd, std::string& error) {
  fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) {
    error = "音频socket失败";
    return -1;
  }
  int one = 1;
  int off = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "音频端口绑定失败";
    ::close(fd);
    fd = -1;
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    error = "音频端口查询失败";
    ::close(fd);
    fd = -1;
    return -1;
  }
  return ntohs(addr.sin6_port);
}

}  // namespace

ByteBuffer hkdf_sha512_labeled(const ByteBuffer& ikm, const char* salt, const char* info) {
#if !defined(CP_HAVE_OPENSSL)
  (void)ikm;
  (void)salt;
  (void)info;
  return {};
#else
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
#endif
}

TimingSync::TimingSync() = default;
TimingSync::~TimingSync() { stop(); }

bool TimingSync::listen(int& port, std::string& error) {
  stop();
  fd_ = ::socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    error = "timing socket failed";
    return false;
  }
  int off = 0;
  ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = 0;
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "timing bind failed";
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port = ntohs(addr.sin6_port);
  clock_offset_ns_ = 0;
  stop_ = false;
  thread_ = std::thread([this] { reader_main(); });
  return true;
}

void TimingSync::start(const std::string& peer_host, int peer_port) {
  std::lock_guard<std::mutex> lock(mu_);
  peer_host_ = normalize_host(peer_host);
  peer_port_ = peer_port;
  started_ = peer_port > 0 && !peer_host_.empty();
}

void TimingSync::stop() {
  stop_ = true;
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  started_ = false;
}

std::uint64_t TimingSync::now_ntp() const { return synced_ntp(); }

std::uint64_t TimingSync::synced_ntp() const {
  using namespace std::chrono;
  const auto mono = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  const auto wall = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  const auto ns = wall + clock_offset_ns_;
  (void)mono;
  const auto sec = static_cast<std::uint64_t>(ns / 1000000000LL) + kNtpEpoch;
  const auto frac = static_cast<std::uint64_t>(((ns % 1000000000LL) << 32) / 1000000000LL);
  return (sec << 32) | frac;
}

void TimingSync::send_request() {
  std::string host;
  int port = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;
    host = peer_host_;
    port = peer_port_;
  }
  std::uint8_t pkt[32] = {};
  pkt[0] = 0x80;
  pkt[1] = kPtRequest;
  pkt[2] = 0;
  pkt[3] = 7;
  const auto t1 = synced_ntp();
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_t1_ = t1;
    have_pending_ = true;
  }
  write_ntp(pkt + 24, t1);
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) != 1) {
    sockaddr_in v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4.sin_addr) != 1) return;
    char mapped[64];
    std::snprintf(mapped, sizeof(mapped), "::ffff:%s", host.c_str());
    if (inet_pton(AF_INET6, mapped, &addr.sin6_addr) != 1) return;
  }
  ::sendto(fd_, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

void TimingSync::reader_main() {
  auto last_send = std::chrono::steady_clock::now() - std::chrono::seconds(2);
  while (!stop_ && fd_ >= 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_send >= std::chrono::seconds(1)) {
      send_request();
      last_send = now;
    }
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, 200) <= 0) continue;
    std::uint8_t msg[64];
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    const ssize_t n = ::recvfrom(fd_, msg, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n < 32) continue;
    char host[128] = {};
    if (peer.ss_family == AF_INET6) {
      auto* a = reinterpret_cast<sockaddr_in6*>(&peer);
      inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
    } else if (peer.ss_family == AF_INET) {
      auto* a = reinterpret_cast<sockaddr_in*>(&peer);
      inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
    }
    const std::string peer_host = normalize_host(host);
    std::string expect;
    int expect_port = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      expect = peer_host_;
      expect_port = peer_port_;
    }
    if (!expect.empty() && peer_host != expect) continue;

    if (msg[1] == kPtRequest) {
      std::uint8_t resp[32] = {};
      resp[0] = 0x80;
      resp[1] = kPtResponse;
      resp[3] = 7;
      std::memcpy(resp + 8, msg + 24, 8);
      write_ntp(resp + 16, synced_ntp());
      write_ntp(resp + 24, synced_ntp());
      ::sendto(fd_, resp, sizeof(resp), 0, reinterpret_cast<sockaddr*>(&peer), peer_len);
      continue;
    }
    if (msg[1] != kPtResponse) continue;
    if (expect_port > 0) {
      int port = 0;
      if (peer.ss_family == AF_INET6) port = ntohs(reinterpret_cast<sockaddr_in6*>(&peer)->sin6_port);
      else if (peer.ss_family == AF_INET) port = ntohs(reinterpret_cast<sockaddr_in*>(&peer)->sin_port);
      if (port != expect_port) continue;
    }
    const auto t4 = synced_ntp();
    const auto t1 = read_ntp(msg + 8);
    const auto t2 = read_ntp(msg + 16);
    const auto t3 = read_ntp(msg + 24);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!have_pending_ || t1 != pending_t1_) continue;
      have_pending_ = false;
    }
    const double two32 = 4294967296.0;
    const double offset = 0.5 * (static_cast<double>(static_cast<std::int64_t>(t2 - t1)) +
                                 static_cast<double>(static_cast<std::int64_t>(t3 - t4))) /
                          two32;
    clock_offset_ns_ += static_cast<std::int64_t>(offset * 1e9);
    (void)wall_ntp;
  }
}

EventChannel::EventChannel() = default;
EventChannel::~EventChannel() { stop(); }

bool EventChannel::listen(const ByteBuffer& shared, int& port, std::string& error, StatusHandler status,
                          CommandHandler commands) {
  stop();
  shared_ = shared;
  status_ = std::move(status);
  on_command_ = std::move(commands);
  {
    std::lock_guard<std::mutex> lock(client_mu_);
    record_ready_ = false;
    pending_.clear();
    event_cseq_ = 0;
  }
  port = bind_tcp6(listen_fd_, error);
  if (port < 0) return false;
  stop_ = false;
  accept_thread_ = std::thread([this] { accept_main(); });
  return true;
}

void EventChannel::handle_incoming(const std::string& req) {
  const auto header_end = req.find("\r\n\r\n");
  if (header_end == std::string::npos) return;
  std::size_t body_len = 0;
  const auto cl = req.find("Content-Length:");
  if (cl != std::string::npos && cl < header_end) {
    body_len = static_cast<std::size_t>(std::strtoul(req.c_str() + cl + 15, nullptr, 10));
  }
  if (body_len == 0 || req.size() < header_end + 4 + body_len) return;
  const auto* body = reinterpret_cast<const std::uint8_t*>(req.data() + header_end + 4);
  Plist root;
  if (!decode_bplist(body, body_len, root)) return;
  const auto* dict = root.as_dict();
  if (!dict) return;
  const auto type_it = dict->find("type");
  if (type_it == dict->end()) return;
  const std::string type = type_it->second.as_string();
  if (type.empty() || !on_command_) return;
  ByteBuffer raw(body, body + body_len);
  on_command_(type, raw);
}

void EventChannel::stop() {
  stop_ = true;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (client_fd_ >= 0) {
    ::shutdown(client_fd_, SHUT_RDWR);
    ::close(client_fd_);
    client_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  if (client_thread_.joinable()) client_thread_.join();
}

void EventChannel::accept_main() {
  while (!stop_ && listen_fd_ >= 0) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    if (::poll(&pfd, 1, 200) <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    if (client_fd_ >= 0) {
      ::close(fd);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(client_mu_);
      client_fd_ = fd;
    }
    if (status_) status_("事件通道已连接");
    if (client_thread_.joinable()) client_thread_.join();
    client_thread_ = std::thread([this, fd] { client_main(fd); });
  }
}

void EventChannel::write_locked(const ByteBuffer& body) {
  if (!cipher_ || client_fd_ < 0) return;
  ++event_cseq_;
  std::string msg = "POST /command RTSP/1.0\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nCSeq: " + std::to_string(event_cseq_) + "\r\n\r\n";
  msg.append(reinterpret_cast<const char*>(body.data()), body.size());
  const std::string wire = cipher_->seal(msg);
  if (!wire.empty()) ::send(client_fd_, wire.data(), wire.size(), MSG_NOSIGNAL);
}

void EventChannel::flush_locked() {
  if (!record_ready_ || !cipher_ || client_fd_ < 0) return;
  for (const auto& body : pending_) write_locked(body);
  pending_.clear();
}

bool EventChannel::send_command(const ByteBuffer& body) {
  std::lock_guard<std::mutex> lock(client_mu_);
  if (!record_ready_ || !cipher_ || client_fd_ < 0) {
    if (pending_.size() >= 64) pending_.erase(pending_.begin());
    pending_.push_back(body);
    return true;
  }
  write_locked(body);
  return true;
}

void EventChannel::mark_record_ready() {
  std::lock_guard<std::mutex> lock(client_mu_);
  record_ready_ = true;
  flush_locked();
}

void EventChannel::client_main(int fd) {
  const ByteBuffer write_key = hkdf_sha512_labeled(shared_, "Events-Salt", "Events-Write-Encryption-Key");
  const ByteBuffer read_key = hkdf_sha512_labeled(shared_, "Events-Salt", "Events-Read-Encryption-Key");
  {
    std::lock_guard<std::mutex> lock(client_mu_);
    cipher_ = std::make_unique<ControlCipher>(read_key, write_key);
    flush_locked();
  }
  std::string enc;
  std::string plain;
  std::uint8_t buf[4096];
  while (!stop_ && fd >= 0) {
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 200) <= 0) continue;
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    enc.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
    std::string opened;
    {
      std::lock_guard<std::mutex> lock(client_mu_);
      if (!cipher_) break;
      opened = cipher_->open(enc);
    }
    plain += opened;
    while (true) {
      const auto header_end = plain.find("\r\n\r\n");
      if (header_end == std::string::npos) break;
      std::size_t body_len = 0;
      const auto cl = plain.find("Content-Length:");
      if (cl != std::string::npos && cl < header_end) {
        body_len = static_cast<std::size_t>(std::strtoul(plain.c_str() + cl + 15, nullptr, 10));
      }
      if (plain.size() < header_end + 4 + body_len) break;
      const std::string req = plain.substr(0, header_end + 4 + body_len);
      plain.erase(0, header_end + 4 + body_len);
      handle_incoming(req);
      std::string cseq = "1";
      const auto cseq_at = req.find("CSeq:");
      if (cseq_at != std::string::npos) {
        cseq.clear();
        for (std::size_t i = cseq_at + 5; i < req.size() && req[i] != '\r'; ++i) {
          if (req[i] != ' ') cseq.push_back(req[i]);
        }
      }
      std::string protocol = "RTSP/1.0";
      const auto line_end = req.find("\r\n");
      if (line_end != std::string::npos) {
        const auto space = req.rfind(' ', line_end);
        if (space != std::string::npos) protocol = req.substr(space + 1, line_end - space - 1);
      }
      const std::string response = protocol + " 200 OK\r\nCSeq: " + cseq + "\r\nContent-Length: 0\r\n\r\n";
      std::lock_guard<std::mutex> lock(client_mu_);
      if (!cipher_ || client_fd_ < 0) break;
      const std::string wire = cipher_->seal(response);
      if (!wire.empty()) ::send(client_fd_, wire.data(), wire.size(), MSG_NOSIGNAL);
    }
  }
  {
    std::lock_guard<std::mutex> lock(client_mu_);
    cipher_.reset();
    if (client_fd_ == fd) client_fd_ = -1;
  }
  ::close(fd);
}

ByteBuffer chacha_open_aad(const ByteBuffer& key, const ByteBuffer& nonce, const std::uint8_t* data, std::size_t len,
                           const std::uint8_t* aad, std::size_t aad_len) {
#if !defined(CP_HAVE_OPENSSL)
  (void)key;
  (void)nonce;
  (void)data;
  (void)len;
  (void)aad;
  (void)aad_len;
  return {};
#else
  if (key.size() != 32 || nonce.size() != 12 || len < 16) return {};
  const std::size_t cipher_len = len - 16;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return {};
  int out_len = 0;
  int extra = 0;
  int final_len = 0;
  ByteBuffer plain(cipher_len);
  const bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
                  EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
                  (aad_len == 0 || EVP_DecryptUpdate(ctx, nullptr, &extra, aad, static_cast<int>(aad_len)) == 1) &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, const_cast<std::uint8_t*>(data + cipher_len)) == 1 &&
                  EVP_DecryptUpdate(ctx, plain.data(), &out_len, data, static_cast<int>(cipher_len)) == 1 &&
                  EVP_DecryptFinal_ex(ctx, plain.data() + out_len, &final_len) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return {};
  plain.resize(static_cast<std::size_t>(out_len + final_len));
  return plain;
#endif
}

ByteBuffer nonce_counter(std::uint64_t counter) {
  ByteBuffer nonce(12, 0);
  for (int i = 0; i < 8; ++i) nonce[static_cast<std::size_t>(4 + i)] = static_cast<std::uint8_t>((counter >> (8 * i)) & 0xff);
  return nonce;
}

ScreenPort::ScreenPort() = default;
ScreenPort::~ScreenPort() { stop(); }

bool ScreenPort::listen(const ByteBuffer& key, int& port, std::string& error, StatusHandler status,
                        FrameHandler frames) {
  stop();
  if (key.size() != 32) {
    error = "视频密钥无效";
    return false;
  }
  key_ = key;
  status_ = std::move(status);
  on_frame_ = std::move(frames);
  frames_ = 0;
  config_bytes_ = 0;
  rx_bytes_window_ = 0;
  rx_bytes_total_ = 0;
  rx_frames_window_ = 0;
  idr_seen_ = 0;
  rx_window_start_ = std::chrono::steady_clock::now();
  // Per-frame disk writes on the TCP thread collapsed phone AverageTxCapacity to ~1 Mbps.
  dump_h264_ = std::getenv("CARPLAY_DUMP_H264") != nullptr;
  const char* home = std::getenv("HOME");
  path_ = std::string(home && *home ? home : ".") + "/.config/carplay-cpp/screen.h264";
  if (dump_h264_) {
    std::ofstream trunc(path_, std::ios::binary | std::ios::trunc);
  }
  port = bind_tcp6(listen_fd_, error);
  if (port < 0) return false;
  stop_ = false;
  thread_ = std::thread([this] { accept_main(); });
  return true;
}

void ScreenPort::set_connected_handler(std::function<void()> handler) { on_connected_ = std::move(handler); }

void ScreenPort::stop() {
  stop_ = true;
  const int active = client_fd_.exchange(-1);
  if (active >= 0) ::shutdown(active, SHUT_RDWR);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
}

void ScreenPort::accept_main() {
  while (!stop_ && listen_fd_ >= 0) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    if (::poll(&pfd, 1, 200) <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    // Large receive buffer + Nagle off so the phone can measure real Wi-Fi capacity
    // (missing AverageTxCapacity → iPhone encodes at the ~1 Mbps floor).
    const int rcv = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    client_fd_ = fd;
    if (status_) status_("视频TCP通道已连接");
    counter_ = 0;
    frames_ = 0;
    rx_bytes_window_ = 0;
    rx_bytes_total_ = 0;
    rx_frames_window_ = 0;
    idr_seen_ = 0;
    rx_window_start_ = std::chrono::steady_clock::now();
    if (on_connected_) on_connected_();
    client_main(fd);
    int expected = fd;
    client_fd_.compare_exchange_strong(expected, -1);
    ::close(fd);
    if (status_) status_("视频通道结束 帧" + std::to_string(frames_.load()));
  }
}

void ScreenPort::append_annexb(ByteBuffer& out, const std::uint8_t* nal, std::size_t len) {
  if (nal == nullptr || len == 0) return;
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(1);
  out.insert(out.end(), nal, nal + len);
}

void ScreenPort::note_rx(std::size_t bytes) {
  rx_bytes_window_ += bytes;
  rx_bytes_total_ += bytes;
}

void ScreenPort::maybe_log_bitrate() {
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - rx_window_start_).count();
  if (ms < 1000) return;
  const double mbps = (static_cast<double>(rx_bytes_window_) * 8.0) / (static_cast<double>(ms) * 1000.0);
  const double fps = (static_cast<double>(frames_.load()) > 0 && ms > 0)
                         ? (static_cast<double>(rx_frames_window_) * 1000.0) / static_cast<double>(ms)
                         : 0.0;
  if (status_) {
    char line[160];
    std::snprintf(line, sizeof(line), "视频码率 %.2f Mbps / %.0f fps / 帧%d / IDR%d", mbps, fps, frames_.load(),
                  idr_seen_);
    status_(line);
  }
  rx_bytes_window_ = 0;
  rx_frames_window_ = 0;
  rx_window_start_ = now;
}

void ScreenPort::emit_frame(const ByteBuffer& annexb) {
  if (annexb.empty() || !on_frame_) return;
  on_frame_(annexb);
}

void ScreenPort::write_avcc(const std::uint8_t* data, std::size_t len) {
  const std::uint8_t* rec = data;
  std::size_t n = len;
  for (std::size_t i = 0; i + 8 <= len; ++i) {
    if (std::memcmp(data + i, "avcC", 4) == 0) {
      rec = data + i + 4;
      n = len - (i + 4);
      break;
    }
  }
  if (n < 7) return;
  ByteBuffer unit;
  std::size_t p = 5;
  const int sps_count = rec[p] & 0x1f;
  ++p;
  for (int i = 0; i < sps_count && p + 2 <= n; ++i) {
    const std::size_t sps_len = (static_cast<std::size_t>(rec[p]) << 8) | rec[p + 1];
    p += 2;
    if (p + sps_len > n) return;
    append_annexb(unit, rec + p, sps_len);
    p += sps_len;
  }
  if (p >= n) return;
  const int pps_count = rec[p++];
  for (int i = 0; i < pps_count && p + 2 <= n; ++i) {
    const std::size_t pps_len = (static_cast<std::size_t>(rec[p]) << 8) | rec[p + 1];
    p += 2;
    if (p + pps_len > n) return;
    append_annexb(unit, rec + p, pps_len);
    p += pps_len;
  }
  config_bytes_ = static_cast<int>(n);
  if (dump_h264_ && !unit.empty()) {
    std::lock_guard<std::mutex> lock(file_mu_);
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (out) out.write(reinterpret_cast<const char*>(unit.data()), static_cast<std::streamsize>(unit.size()));
  }
  emit_frame(unit);
}

void ScreenPort::on_message(const std::uint8_t* header, const std::uint8_t* body, std::size_t body_len, std::string& error) {
  note_rx(128 + body_len);
  const std::uint8_t opcode = header[4];
  if (opcode == 1) {
    write_avcc(body, body_len);
    if (status_) status_("视频配置 h264 " + std::to_string(config_bytes_) + "B");
    return;
  }
  if (opcode != 0) return;
  if (body_len < 16) {
    error = "视频帧过短";
    return;
  }
  const ByteBuffer plain = chacha_open_aad(key_, nonce_counter(counter_), body, body_len, header, 128);
  if (plain.empty()) {
    error = "视频帧认证失败";
    return;
  }
  ++counter_;
  std::size_t off = 0;
  int nals = 0;
  bool is_idr = false;
  ByteBuffer unit;
  while (off + 4 <= plain.size()) {
    const std::size_t nal_len = (static_cast<std::size_t>(plain[off]) << 24) | (static_cast<std::size_t>(plain[off + 1]) << 16) |
                                (static_cast<std::size_t>(plain[off + 2]) << 8) | plain[off + 3];
    off += 4;
    if (nal_len == 0 || off + nal_len > plain.size()) break;
    if ((plain[off] & 0x1f) == 5) is_idr = true;
    append_annexb(unit, plain.data() + off, nal_len);
    off += nal_len;
    ++nals;
  }
  if (is_idr) ++idr_seen_;
  if (dump_h264_ && !unit.empty()) {
    std::lock_guard<std::mutex> lock(file_mu_);
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (out) out.write(reinterpret_cast<const char*>(unit.data()), static_cast<std::streamsize>(unit.size()));
  }
  emit_frame(unit);
  ++frames_;
  ++rx_frames_window_;
  if (is_idr && status_) {
    status_("关键帧 " + std::to_string(unit.size()) + "B");
  }
  maybe_log_bitrate();
}

void ScreenPort::client_main(int fd) {
  std::vector<std::uint8_t> acc;
  acc.reserve(256 * 1024);
  std::size_t head = 0;
  std::uint8_t buf[65536];
  while (!stop_) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    if (head > 0) {
      acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(head));
      head = 0;
    }
    acc.insert(acc.end(), buf, buf + n);
    if (acc.size() > 16 * 1024 * 1024) break;
    while (acc.size() - head >= 128) {
      const auto* raw = acc.data() + head;
      const std::uint32_t body_size = static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
                                      (static_cast<std::uint32_t>(raw[2]) << 16) | (static_cast<std::uint32_t>(raw[3]) << 24);
      if (body_size > 8 * 1024 * 1024) {
        if (status_) status_("视频包长度无效");
        return;
      }
      if (acc.size() - head < 128 + body_size) break;
      std::string error;
      on_message(raw, raw + 128, body_size, error);
      head += 128 + body_size;
      if (!error.empty()) {
        if (status_) status_(error);
        return;
      }
    }
    if (head > 512 * 1024 && head * 2 > acc.size()) {
      acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(head));
      head = 0;
    }
  }
}

}  // namespace carplay
