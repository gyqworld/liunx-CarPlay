#include "carplay/platform/linux_hotspot.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>

namespace carplay {
namespace {

std::string shell_quote(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

std::string run_capture(const std::string& command, int& code) {
  std::array<char, 512> buf{};
  std::string out;
  FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
  if (!pipe) {
    code = 127;
    return "popen failed";
  }
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
  code = ::pclose(pipe);
  if (WIFEXITED(code)) code = WEXITSTATUS(code);
  return out;
}

std::string script_path() {
  if (const char* env = std::getenv("CARPLAY_HOTSPOT_SCRIPT"); env && *env) return env;
  const char* system = "/usr/local/sbin/carplay-hotspot";
  if (::access(system, X_OK) == 0) return system;
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.local/bin/carplay-hotspot";
  }
  return system;
}

std::string sudo_prefix() {
  // sudoers grants NOPASSWD only for this script. A password prompt would hang popen.
  return "sudo -n ";
}

}  // namespace

bool start_carplay_hotspot(const HotspotRequest& req, std::string& error) {
  if (req.ssid.empty() || req.password.size() < 8) {
    error = "热点名称不能为空，密码至少 8 位";
    return false;
  }
  int channel = req.channel;
  if (!req.keep_station && channel != 1 && channel != 6 && channel != 11) channel = 6;
  std::ostringstream cmd;
  cmd << sudo_prefix() << shell_quote(script_path()) << " start"
      << " --iface " << shell_quote(req.iface.empty() ? "wlo1" : req.iface)
      << " --ssid " << shell_quote(req.ssid)
      << " --pass " << shell_quote(req.password)
      << " --channel " << channel;
  if (req.keep_station) cmd << " --alongside";
  if (!req.bt_mac.empty()) cmd << " --bt-mac " << shell_quote(req.bt_mac);
  int code = 0;
  const std::string out = run_capture(cmd.str(), code);
  if (code != 0) {
    error = out.empty() ? ("热点启动失败，exit=" + std::to_string(code)) : out;
    return false;
  }
  error = out;
  return true;
}

bool stop_carplay_hotspot(std::string& error) {
  int code = 0;
  const std::string out = run_capture(sudo_prefix() + shell_quote(script_path()) + " stop", code);
  if (code != 0) {
    error = out.empty() ? "热点停止失败" : out;
    return false;
  }
  error = out;
  return true;
}

HotspotStatus query_carplay_hotspot() {
  HotspotStatus st;
  int code = 0;
  st.detail = run_capture(shell_quote(script_path()) + " status", code);
  while (!st.detail.empty() && (st.detail.back() == '\n' || st.detail.back() == '\r')) st.detail.pop_back();
  st.running = (code == 0);
  return st;
}

}  // namespace carplay
