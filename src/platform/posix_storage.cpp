#include "carplay/platform/storage.hpp"

#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>

namespace carplay {
namespace {

std::string default_root() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg) return std::string(xdg) + "/carplay-cpp";
  }
  if (const char* home = std::getenv("HOME")) {
    if (*home) return std::string(home) + "/.config/carplay-cpp";
  }
  return "./carplay-cpp-data";
}

}  // namespace

PosixStorage::PosixStorage(std::string root) : root_(std::move(root)) {
  if (root_.empty()) root_ = default_root();
}

std::string PosixStorage::data_dir() const { return root_; }

bool PosixStorage::ensure_data_dir() const {
  std::string path;
  for (std::size_t i = 0; i < root_.size(); ++i) {
    path.push_back(root_[i]);
    if (root_[i] != '/' && i + 1 != root_.size()) continue;
    if (path == "/") continue;
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
  }
  struct stat st {};
  return ::stat(root_.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace carplay
