#include "carplay/session/receiver.hpp"

#include "carplay/media/frame_clock.hpp"
#include "carplay/protocol/pair_setup.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif

namespace carplay {
namespace {

bool auth_files_present(const std::string& dir) {
  if (dir.empty()) return false;
  struct stat key_st {};
  struct stat cert_st {};
  return ::stat((dir + "/identity.pk8").c_str(), &key_st) == 0 &&
         ::stat((dir + "/certificate.p7b").c_str(), &cert_st) == 0;
}

std::string bundled_auth_dir() {
  const char* candidates[] = {"resources/authentication", "../resources/authentication",
                              "../../resources/authentication"};
  for (const char* path : candidates) {
    if (auth_files_present(path)) return path;
  }
#if !defined(__APPLE__)
  char exe[4096];
  const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = 0;
    std::string dir(exe);
    const auto slash = dir.find_last_of('/');
    if (slash != std::string::npos) dir.resize(slash);
    if (auth_files_present(dir + "/resources/authentication")) return dir + "/resources/authentication";
    if (auth_files_present(dir + "/../resources/authentication")) return dir + "/../resources/authentication";
  }
#endif
  return {};
}

std::string mac_from_bytes(const unsigned char* data, int len) {
  if (len < 6) return {};
  char text[18];
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", data[0], data[1], data[2], data[3], data[4],
                data[5]);
  return text;
}

}  // namespace

Receiver::Receiver(std::shared_ptr<IBluetoothTransport> bt, std::shared_ptr<IWifiAccessPoint> wifi)
    : bt_(std::move(bt)), wifi_(std::move(wifi)) {}

Receiver::~Receiver() { stop(); }

void Receiver::publish(const std::string& status) {
  StatusHandler handler;
  {
    std::lock_guard<std::mutex> lock(status_mu_);
    handler = status_;
  }
  if (handler) handler(status);
}

void Receiver::post_touch(float x, float y, bool down) {
  std::lock_guard<std::mutex> lock(touch_mu_);
  if (!accepting_touch_) return;
  if (touches_.size() >= 256) touches_.erase(touches_.begin());
  touches_.push_back(TouchPoint{x, y, down});
  if (wake_w_ >= 0) {
    const char b = 1;
    ::write(wake_w_, &b, 1);
  }
}

void Receiver::reset_local() {
  {
    std::lock_guard<std::mutex> lock(touch_mu_);
    accepting_touch_ = false;
    stop_ = true;
    if (wake_w_ >= 0) {
      const char b = 1;
      ::write(wake_w_, &b, 1);
    }
  }
  if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
  if (thread_.joinable()) thread_.join();
  if (wake_r_ >= 0) {
    ::close(wake_r_);
    wake_r_ = -1;
  }
  if (wake_w_ >= 0) {
    ::close(wake_w_);
    wake_w_ = -1;
  }
  {
    std::lock_guard<std::mutex> lock(touch_mu_);
    touches_.clear();
  }
  for (auto& client : clients_) {
    if (client && client->fd >= 0) ::close(client->fd);
  }
  clients_.clear();
  control_count_ = 0;
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  {
    std::lock_guard<std::mutex> lock(link_mu_);
    if (accessory_) accessory_->close();
    accessory_.reset();
    link_.reset();
    signer_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(status_mu_);
    status_ = nullptr;
  }
  stop_ = false;
}

bool Receiver::read_interface(std::string& error) {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    error = "所选接口没有可用地址，请检查移动热点或局域网连接";
    return false;
  }
  std::string ipv4;
  std::string mac;
  for (ifaddrs* it = list; it; it = it->ifa_next) {
    if (!it->ifa_addr || (it->ifa_flags & IFF_LOOPBACK)) continue;
    if (!config_.network_interface.empty() && config_.network_interface != it->ifa_name) continue;
    if (it->ifa_addr->sa_family == AF_INET && ipv4.empty()) {
      char text[INET_ADDRSTRLEN] = {};
      auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
      if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) && std::strcmp(text, "127.0.0.1") != 0) {
        ipv4 = text;
        if (config_.network_interface.empty()) config_.network_interface = it->ifa_name;
      }
    }
#if defined(__APPLE__)
    if (it->ifa_addr->sa_family == AF_LINK && mac.empty()) {
      auto* link = reinterpret_cast<sockaddr_dl*>(it->ifa_addr);
      mac = mac_from_bytes(reinterpret_cast<const unsigned char*>(LLADDR(link)), link->sdl_alen);
    }
#else
    if (it->ifa_addr->sa_family == AF_PACKET && mac.empty()) {
      auto* link = reinterpret_cast<sockaddr_ll*>(it->ifa_addr);
      mac = mac_from_bytes(link->sll_addr, link->sll_halen);
    }
#endif
  }
  freeifaddrs(list);
  if (ipv4.empty()) {
    error = "所选接口没有可用地址，请检查移动热点或局域网连接";
    return false;
  }
  handoff_.host = ipv4;
  if (!config_.access_point_mac.empty())
    handoff_.ap_mac = config_.access_point_mac;
  else if (!mac.empty())
    handoff_.ap_mac = mac;
  else
    handoff_.ap_mac = "02:00:00:00:00:02";
  return true;
}

bool Receiver::listen_port(std::string& error) {
  listen_fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    error = "TCP17000接收端口不可用，请检查端口占用";
    return false;
  }
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int off = 0;
  ::setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(static_cast<std::uint16_t>(config_.port > 0 ? config_.port : 17000));
  addr.sin6_addr = in6addr_any;
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 8) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    error = "TCP17000接收端口不可用，请检查端口占用";
    return false;
  }
  return true;
}

bool Receiver::start(const RuntimeConfig& config, std::string& error, StatusHandler status, FrameHandler frames,
                     HostUiHandler host_ui) {
  reset_local();
  config_ = config;
  {
    std::lock_guard<std::mutex> lock(status_mu_);
    status_ = std::move(status);
    on_frame_ = std::move(frames);
    on_host_ui_ = std::move(host_ui);
  }
  handoff_ = Handoff{};
  handoff_.name = config_.device_name.empty() ? "专业拧瓶盖员" : config_.device_name;
  handoff_.ssid = config_.ssid;
  handoff_.password = config_.password;
  handoff_.channel = config_.channel;
  handoff_.port = config_.port > 0 ? config_.port : 17000;
  handoff_.pk = pairing_public_hex();
  handoff_.serial = "CARPLAYCPP";
  handoff_.bt_mac = config_.adapter_bluetooth.empty() ? "02:00:00:00:00:02" : config_.adapter_bluetooth;

  std::string auth_dir = config_.mfi_dir;
  if (auth_dir.empty()) {
    const char* home = std::getenv("HOME");
    auth_dir = std::string(home && *home ? home : ".") + "/.config/carplay-cpp/authentication";
  }
  auto file_signer = std::make_unique<FileMfiSigner>(auth_dir);
  if (!file_signer->available()) {
    const std::string bundled = bundled_auth_dir();
    if (!bundled.empty()) file_signer = std::make_unique<FileMfiSigner>(bundled);
  }
  if (!file_signer->available()) {
    error = file_signer->error();
    return false;
  }
  signer_ = std::move(file_signer);
  if (!read_interface(error)) return false;

  WifiAccessPointInfo published;
  published.ssid = config_.ssid;
  published.password = config_.password;
  published.channel = config_.channel;
  published.interface_name = config_.network_interface;
  if (wifi_ && !wifi_->apply(published)) {
    const auto current = wifi_->query();
    if (!current.ready) {
      error = "network is not ready";
      return false;
    }
  }
  if (!listen_port(error)) return false;
  publish("网络接收端已就绪");
  publish("连接iPhone蓝牙RFCOMM服务");
  if (!bt_) {
    error = "蓝牙传输不可用";
    reset_local();
    return false;
  }
  bt_->set_data_handler([this](const std::uint8_t* data, std::size_t len) {
    std::lock_guard<std::mutex> lock(link_mu_);
    if (link_) link_->feed(data, len);
  });
  if (!bt_->connect_rfcomm(config_.target_bluetooth)) {
    error = bt_->last_error().empty() ? "蓝牙RFCOMM连接失败" : bt_->last_error();
    reset_local();
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(link_mu_);
    link_ = std::make_unique<IapLink>([this](const std::uint8_t* data, std::size_t len) {
      if (bt_) bt_->write(data, len);
    });
    accessory_ = std::make_unique<Accessory>(*link_, *signer_, handoff_);
    accessory_->on_status = [this](const std::string& text) { publish(text); };
    accessory_->on_failed = [this](const std::string& text) {
      publish(text);
      if (control_count_.load() == 0) stop_ = true;
    };
    accessory_->on_negotiation = [this] { publish("已发送CarPlay参数，等待iPhone网络连接"); };
    const auto previous_failed = link_->on_failed;
    link_->on_failed = [this, previous_failed](const std::string& text) {
      if (control_count_.load() > 0 && text.find("确认超时") != std::string::npos) {
        publish("蓝牙确认超时，控制连接已保留");
        return;
      }
      if (previous_failed) previous_failed(text);
      else publish(text);
      if (control_count_.load() == 0) stop_ = true;
    };
  }

  publish("iAP2检测标记已发送，等待手机应答");
  publish(config_.wireless_mode == WirelessMode::Lan ? "局域网接收已启动，请确认两端在同一Wi-Fi"
                                                      : "本机热点接收已启动，请让iPhone接入本机热点");
  int pipes[2] = {-1, -1};
  if (::pipe(pipes) == 0) {
    ::fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    ::fcntl(pipes[1], F_SETFL, O_NONBLOCK);
    wake_r_ = pipes[0];
    wake_w_ = pipes[1];
  }
  {
    std::lock_guard<std::mutex> lock(touch_mu_);
    accepting_touch_ = true;
  }
  thread_ = std::thread([this] { thread_main(); });
  return true;
}

void Receiver::thread_main() {
  while (!stop_) {
    std::vector<pollfd> fds;
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    for (const auto& client : clients_) fds.push_back(pollfd{client->fd, POLLIN, 0});
    if (wake_r_ >= 0) fds.push_back(pollfd{wake_r_, POLLIN, 0});
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
    if (wake_r_ >= 0) {
      char dump[64];
      while (::read(wake_r_, dump, sizeof(dump)) > 0) {
      }
    }
    std::vector<TouchPoint> batch;
    {
      std::lock_guard<std::mutex> lock(touch_mu_);
      batch.swap(touches_);
    }
    for (const auto& point : batch) {
      for (auto& client : clients_) client->session.send_touch(point.x, point.y, point.down);
    }
    const auto now = static_cast<std::uint64_t>(FrameClock::now_us() / 1000);
    {
      std::lock_guard<std::mutex> lock(link_mu_);
      if (link_) link_->tick(now);
      if (accessory_) accessory_->tick(now);
    }
    if (stop_) break;

    if (!fds.empty() && (fds[0].revents & POLLIN)) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      const int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (client >= 0) {
        auto item = std::make_unique<ControlClient>();
        item->fd = client;
        item->session.set_signer(signer_.get());
        item->session.set_config(config_);
        item->session.set_status_handler([this](const std::string& text) { publish(text); });
        {
          std::lock_guard<std::mutex> lock(status_mu_);
          item->session.set_frame_handler(on_frame_);
          item->session.set_host_ui_handler(on_host_ui_);
        }
        char host[128] = {};
        if (peer.ss_family == AF_INET6) {
          auto* a = reinterpret_cast<sockaddr_in6*>(&peer);
          ::inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        } else if (peer.ss_family == AF_INET) {
          auto* a = reinterpret_cast<sockaddr_in*>(&peer);
          ::inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        }
        std::string peer_host = host;
        if (peer_host.rfind("::ffff:", 0) == 0) peer_host = peer_host.substr(7);
        item->session.set_peer_host(peer_host);
        clients_.push_back(std::move(item));
        control_count_.store(static_cast<int>(clients_.size()));
        publish("网络控制连接已到达");
      }
    }

    for (std::size_t i = 0; i < clients_.size();) {
      auto& client = *clients_[i];
      if (i + 1 >= fds.size() || (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        ++i;
        continue;
      }
      auto drop_client = [&] {
        ::close(client.fd);
        clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
        control_count_.store(static_cast<int>(clients_.size()));
        if (clients_.empty()) {
          publish("iPhone 控制连接已断开");
          HostUiHandler leave;
          {
            std::lock_guard<std::mutex> lock(status_mu_);
            leave = on_host_ui_;
          }
          if (leave) leave();
        }
      };
      if (fds[i + 1].revents & (POLLHUP | POLLERR)) {
        drop_client();
        continue;
      }
      std::uint8_t buf[4096];
      const ssize_t n = ::recv(client.fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        drop_client();
        continue;
      }
      const auto responses = client.session.feed(buf, static_cast<std::size_t>(n));
      if (!responses.empty() && !client.session.last_request_line().empty()) {
        publish(std::string("控制请求 ") + client.session.last_request_line());
      }
      bool ok = true;
      for (const auto& response : responses) {
        std::size_t sent = 0;
        while (sent < response.size()) {
          const ssize_t w = ::send(client.fd, response.data() + sent, response.size() - sent, 0);
          if (w <= 0) {
            ok = false;
            break;
          }
          sent += static_cast<std::size_t>(w);
        }
        if (!ok) break;
      }
      if (!ok) {
        drop_client();
        continue;
      }
      ++i;
    }
    control_count_.store(static_cast<int>(clients_.size()));
  }
}

void Receiver::stop() {
  reset_local();
  if (bt_) {
    bt_->set_data_handler(nullptr);
    bt_->disconnect();
  }
}

}  // namespace carplay
